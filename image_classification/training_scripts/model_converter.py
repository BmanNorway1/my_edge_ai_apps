'''
MLCommons
group: TinyMLPerf (https://github.com/mlcommons/tiny)

model_converter.py: converts trained floating point model to fully int8 quantized TFLite.

Modified from original CIFAR-10 version:
- Finds the most recently trained model automatically (trainedResnet_*.h5)
- Uses finger digits dataset for int8 calibration instead of CIFAR-10
'''

import glob
import os
import numpy as np
import tensorflow as tf
from train import load_finger_digits, DATA_DIR, IMG_SIZE

# Find the most recently modified .h5 file, regardless of which architecture
# it was trained as (train.py now saves as trainedEIStyle_* or trainedResnet_*
# depending on the ARCHITECTURE setting).
candidates = glob.glob('trained_models/*.h5')
if not candidates:
    raise FileNotFoundError("No trained model found in trained_models/. Run train.py first.")

tfmodel_path = max(candidates, key=os.path.getmtime)
model_name = os.path.splitext(os.path.basename(tfmodel_path))[0]
print(f"Using model: {tfmodel_path}")

tfmodel = tf.keras.models.load_model(tfmodel_path)

def representative_dataset_generator():
    images, _, _, _ = load_finger_digits(DATA_DIR, img_size=IMG_SIZE)
    # Use up to 200 samples for calibration
    indices = np.random.choice(len(images), size=min(200, len(images)), replace=False)
    for i in indices:
        sample = np.expand_dims(images[i].astype(np.float32), axis=0)
        yield [sample]

if __name__ == '__main__':
    # Float TFLite model
    converter = tf.lite.TFLiteConverter.from_keras_model(tfmodel)
    tflite_model = converter.convert()
    float_path = f'trained_models/{model_name}.tflite'
    open(float_path, 'wb').write(tflite_model)
    print(f"Float model saved to {float_path}")

    # Fully quantized int8 TFLite model (needed for Axon compiler)
    converter = tf.lite.TFLiteConverter.from_keras_model(tfmodel)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.representative_dataset = representative_dataset_generator
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    tflite_quant_model = converter.convert()
    quant_path = f'trained_models/{model_name}_quant.tflite'
    open(quant_path, 'wb').write(tflite_quant_model)
    print(f"Quantized model saved to {quant_path}")
