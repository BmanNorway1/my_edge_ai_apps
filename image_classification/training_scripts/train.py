'''
Adapted from MLCommons TinyMLPerf image classification template.

Dataset: Finger digits, flat folder of PNGs named {class}_{sample}.png
  e.g. 3_0.png means class 3, sample 0
  The integer before the underscore is the class label.
  Classes 0-5 are digits, class 6 is "unknown" (e.g. 6_0.png, 6_1.png, ...)

train.py: loads data, trains and saves model, plots training metrics
'''

import os
import re
import numpy as np
import matplotlib.pyplot as plt
import tensorflow as tf
from tensorflow.keras.callbacks import LearningRateScheduler
from tensorflow.keras.utils import to_categorical
from PIL import Image
import keras_model
import datetime

# ── Config ────────────────────────────────────────────────────────────────────
DATA_DIR   = 'training_images'   # flat folder containing all {class}_{sample}.png files (classes 0-6)
IMG_SIZE   = 96                 # resize all images to IMG_SIZE x IMG_SIZE
VAL_SPLIT  = 0.2

# Which model architecture to train.
#   'ei_style' -> lightweight CNN matching the Edge Impulse Studio model
#                 (Conv2D(16) -> Conv2D(32) -> Flatten -> Dropout -> Dense)
#   'resnet'   -> the original deeper resnet_v1_eembc architecture
ARCHITECTURE = 'ei_style'

# If True, train using Edge Impulse Studio's exact settings (10 epochs, fixed
# learning rate, batch size 32, no learning-rate decay) so results are
# directly comparable to the Edge Impulse-trained model. If False, use the
# alternate settings below (matches the original TinyMLPerf-style regime:
# longer training with a decaying learning rate).
MATCH_EI_HYPERPARAMS = True

if MATCH_EI_HYPERPARAMS:
    EPOCHS          = 10
    BS              = 32
    LEARNING_RATE   = 0.0005   # fixed, no decay
    USE_LR_SCHEDULE = False
else:
    EPOCHS          = 80
    BS              = 48
    LEARNING_RATE   = 0.0001   # initial rate for the decay schedule below
    USE_LR_SCHEDULE = True

# 'grayscale' or 'rgb'. Controls both the number of channels the model is
# built with and how load_finger_digits() prepares the data.
#   'grayscale' -> 1-channel input, matches the greyscale on-device pipeline
#                  documented elsewhere (train/serve pixel-value parity).
#   'rgb'       -> 3-channel input, full color. NOTE: if you plan to deploy an
#                  RGB model through this same manual-driver firmware (not the
#                  Edge Impulse SDK path), the firmware's own preprocessing
#                  will need to convert HWC camera data to the CHW layout the
#                  compiled Axon model expects -- the same class of bug that
#                  had to be patched in the Edge Impulse SDK wrapper. This is
#                  a firmware-side concern, not something train.py handles.
COLOR_MODE = 'grayscale'

# Only used when COLOR_MODE == 'grayscale'.
# If True, replicate the firmware's exact grayscale conversion (including the
# RGB565 color-depth reduction the camera hardware itself performs) via
# rgb888_to_camera_gray(). This is the recommended setting, since it keeps
# training and on-device inference seeing the same pixel values.
# If False, use PIL's built-in convert('L') instead: a similar but not
# bit-identical formula, kept only as a fallback / comparison option.
USE_FIRMWARE_GRAYSCALE = True
# ─────────────────────────────────────────────────────────────────────────────

dt = datetime.datetime.today()
_arch_label = {'ei_style': 'EIStyle', 'resnet': 'Resnet'}[ARCHITECTURE]
model_name = f"trained{_arch_label}_{dt.year}{dt.month:02d}{dt.day:02d}_{dt.hour:02d}{dt.minute:02d}.h5"


# ── Data loading ──────────────────────────────────────────────────────────────

def rgb888_to_camera_gray(img_rgb_uint8):
    """
    Replicates the on-device grayscale conversion exactly.

    Verified against the actual data-collection pipeline (main.c +
    collect_dataset.py): the PNGs in the training folder already store pixel
    values expanded from RGB565 using r5*255//31 / g6*255//63 (see
    rgb565_to_png() in collect_dataset.py), which is the exact same formula
    the inference firmware's rgb565_to_gray() uses. That expansion has been
    confirmed bit-exact and lossless across every possible 5-bit/6-bit input,
    so no re-quantization is needed here: the PNG's stored RGB values are
    already exactly what the camera produced. This function only needs to
    apply the same integer luma weights the firmware uses.

    Input: (H, W, 3) uint8 RGB array.
    Output: (H, W) uint8 array, matching the firmware's rgb565_to_gray().
    """
    r = img_rgb_uint8[..., 0].astype(np.uint32)
    g = img_rgb_uint8[..., 1].astype(np.uint32)
    b = img_rgb_uint8[..., 2].astype(np.uint32)

    # Same integer luma weights as the firmware, applied directly to the
    # already-camera-accurate RGB values stored in the PNG.
    gray = (r * 77 + g * 150 + b * 29) >> 8
    return gray.astype(np.uint8)


def load_finger_digits(data_dir, img_size=128):
    """
    Loads PNGs from a flat folder named {class}_{sample}.png.
    The class label is the integer before the underscore (e.g. 3_0.png -> class 3).
    Returns integer labels, and images as float32 arrays in [0, 255]:
    shape (H, W, 1) if COLOR_MODE == 'grayscale', or (H, W, 3) if 'rgb'.

    When COLOR_MODE == 'grayscale', conversion is controlled by the
    module-level USE_FIRMWARE_GRAYSCALE flag:
      - True (recommended): uses rgb888_to_camera_gray(), which applies the
        firmware's exact integer luma weights to the RGB values already
        stored in the PNG. These PNGs were produced by collect_dataset.py
        from raw RGB565 camera frames, using the same expansion formula the
        inference firmware uses, so the stored pixel values already reflect
        the camera's real RGB565 color-depth reduction; no re-quantization is
        needed here.
      - False: uses PIL's convert('L') instead, a similar but not
        bit-identical formula, kept only as a fallback / comparison option.

    When COLOR_MODE == 'rgb', the image is kept as full 3-channel color and
    USE_FIRMWARE_GRAYSCALE has no effect.
    """
    pattern = re.compile(r'^(\d+)_\d+\.png$', re.IGNORECASE)

    images = []
    labels = []

    filenames = sorted(os.listdir(data_dir))
    for fname in filenames:
        m = pattern.match(fname)
        if not m:
            continue  # skip files that don't match the naming convention

        label = int(m.group(1))
        path  = os.path.join(data_dir, fname)

        # Resize while still in color, THEN convert to grayscale if needed --
        # this matches the device pipeline, where the camera captures/crops
        # to the target resolution first and any grayscale conversion happens
        # on that already-resized frame, not the other way around.
        img = Image.open(path).convert('RGB')
        img = img.resize((img_size, img_size), Image.BILINEAR)

        if COLOR_MODE == 'rgb':
            img_arr = np.array(img, dtype=np.float32)   # (H, W, 3)
        elif COLOR_MODE == 'grayscale':
            if USE_FIRMWARE_GRAYSCALE:
                img_rgb = np.array(img, dtype=np.uint8)   # (H, W, 3)
                gray = rgb888_to_camera_gray(img_rgb)      # (H, W) uint8
            else:
                gray = np.array(img.convert('L'), dtype=np.uint8)  # (H, W)
            img_arr = gray.astype(np.float32)[:, :, np.newaxis]     # (H, W, 1)
        else:
            raise ValueError(f"Unknown COLOR_MODE: {COLOR_MODE!r}")

        images.append(img_arr)
        labels.append(label)

    images = np.stack(images)          # (N, H, W, 1) or (N, H, W, 3)
    labels = np.array(labels)

    # Remap labels to a contiguous range starting at 0
    # (this is a no-op as long as every class 0-6 has at least one sample present)
    unique_labels = np.unique(labels)
    label_map     = {orig: new for new, orig in enumerate(unique_labels)}
    labels        = np.array([label_map[l] for l in labels])
    num_classes   = len(unique_labels)

    print(f"Loaded {len(images)} images, {num_classes} classes: {list(unique_labels)}")
    return images, labels, num_classes, unique_labels


def train_val_split(images, labels, val_fraction=0.2, seed=42):
    rng = np.random.default_rng(seed)
    idx = rng.permutation(len(images))
    split = int(len(images) * (1 - val_fraction))
    train_idx, val_idx = idx[:split], idx[split:]
    return images[train_idx], labels[train_idx], images[val_idx], labels[val_idx]

def save_visualization_samples(images, labels, num_samples=10, output_dir='dataset_visualization'):
    os.makedirs(output_dir, exist_ok=True)

    indices = np.random.choice(len(images), num_samples, replace=False)

    for i, idx in enumerate(indices):
        img = images[idx].astype(np.uint8)
        label = labels[idx]

        # Convert to PIL and save -- mode depends on channel count, not a
        # hardcoded assumption, so this works for both COLOR_MODE settings.
        if img.shape[-1] == 1:
            pil_img = Image.fromarray(img[:, :, 0], mode='L')
        else:
            pil_img = Image.fromarray(img, mode='RGB')
        pil_img = pil_img.resize((256, 256), Image.NEAREST)  # scale up for visibility, NEAREST keeps it crisp binary

        fname = f"sample_{i:02d}_class{label}.png"
        pil_img.save(os.path.join(output_dir, fname))

    print(f"Saved {num_samples} visualization samples to {output_dir}/")


# ── LR schedule ───────────────────────────────────────────────────────────────

def lr_schedule(epoch):
    decay_per_epoch = 0.99
    lrate = LEARNING_RATE * (decay_per_epoch ** epoch)
    print(f'Learning rate = {lrate:.6f}')
    return lrate

lr_scheduler = LearningRateScheduler(lr_schedule)

# ── Data augmentation ─────────────────────────────────────────────────────────

datagen = tf.keras.preprocessing.image.ImageDataGenerator(
    rotation_range=10,
    width_shift_range=0.1,
    height_shift_range=0.1,
    horizontal_flip=False,  # gestures are not left-right symmetric; a
                             # flipped finger-count image can be a different
                             # or invalid gesture, not just a valid variation
    #zoom_range=[0.5, 1.0],
)

# ── Main ──────────────────────────────────────────────────────────────────────
if __name__ == "__main__":

    # Load data (includes both digit classes and unknown_{sample}.png files)
    images, labels, num_classes, unique_labels = load_finger_digits(DATA_DIR, img_size=IMG_SIZE)

    print(f"Total: {len(images)} images, {num_classes} classes")

    train_data, train_labels_raw, val_data, val_labels_raw = train_val_split(
        images, labels, val_fraction=VAL_SPLIT
    )

    save_visualization_samples(images, labels)  

    # One-hot encode
    train_labels = to_categorical(train_labels_raw, num_classes=num_classes)
    val_labels   = to_categorical(val_labels_raw,   num_classes=num_classes)

    print(f"Train: {train_data.shape}, Val: {val_data.shape}")
    print(f"Label names: {unique_labels}")

    # Show a sample grid
    num_plot = 5
    f, ax = plt.subplots(num_plot, num_plot)
    for m in range(num_plot):
        for n in range(num_plot):
            idx = np.random.randint(0, train_data.shape[0])
            if train_data.shape[-1] == 1:
                ax[m, n].imshow(train_data[idx, :, :, 0].astype(np.uint8), cmap='gray')
            else:
                ax[m, n].imshow(train_data[idx].astype(np.uint8))
            ax[m, n].set_title(str(train_labels_raw[idx]), fontsize=6)
            ax[m, n].axis('off')
    f.subplots_adjust(hspace=0.4, wspace=0.1)
    plt.suptitle('Sample training images')
    plt.savefig('sample_grid.png', dpi=100)
    plt.show()

    # Build model — input_shape and num_classes are now proper parameters
    # (see keras_model.py which has been updated from the original hardcoded version)
    num_channels = 3 if COLOR_MODE == 'rgb' else 1
    if ARCHITECTURE == 'ei_style':
        new_model = keras_model.ei_style_cnn(
            input_shape=(IMG_SIZE, IMG_SIZE, num_channels),
            num_classes=num_classes,
        )
    elif ARCHITECTURE == 'resnet':
        new_model = keras_model.resnet_v1_eembc(
            conv_filters=16,
            input_shape=(IMG_SIZE, IMG_SIZE, num_channels),
            num_classes=num_classes,
        )
    else:
        raise ValueError(f"Unknown ARCHITECTURE: {ARCHITECTURE!r}")

    new_model.summary()

    # Fit normalisation statistics on training data
    datagen.fit(train_data)

    new_model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=LEARNING_RATE),
        loss='categorical_crossentropy',
        metrics='accuracy',
    )

    # The LR-schedule callback is only used when MATCH_EI_HYPERPARAMS is
    # False. Edge Impulse trained with a fixed learning rate, so matching it
    # means training without a decay schedule.
    callbacks = [lr_scheduler] if USE_LR_SCHEDULE else []

    history = new_model.fit(
        datagen.flow(train_data, train_labels, batch_size=BS),
	steps_per_epoch=len(train_data) / BS,
	epochs=EPOCHS,
        validation_data=(val_data, val_labels),
        callbacks=callbacks,
    )

    # Plot loss + accuracy
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 4))
    ax1.plot(history.history['loss'],     label='train')
    ax1.plot(history.history['val_loss'], label='val')
    ax1.set_title('Loss'); ax1.legend()

    ax2.plot(history.history['accuracy'],     label='train')
    ax2.plot(history.history['val_accuracy'], label='val')
    ax2.set_title('Accuracy'); ax2.legend()

    plt.tight_layout()
    plt.savefig('train_loss_acc.png')
    plt.show()

    os.makedirs('trained_models', exist_ok=True)
    new_model.save(os.path.join('trained_models', model_name))
    print(f"Model saved to trained_models/{model_name}")
