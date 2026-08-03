'''
MLCommons
group: TinyMLPerf (https://github.com/mlcommons/tiny)

image classification on cifar10
keras_model.py: CIFAR10_ResNetv1 from eembc

Modified: input_shape and num_classes are now parameters so the model
can be used with datasets other than CIFAR-10 (e.g. 128x128 finger digits).
'''

import numpy as np
import os

import tensorflow as tf
from tensorflow.keras.models import Model
from tensorflow.keras.layers import Input, Dense, Activation, Flatten, BatchNormalization
from tensorflow.keras.layers import Conv2D, AveragePooling2D, MaxPooling2D, Dropout
from tensorflow.keras.regularizers import l2


def get_model_name():
    if os.path.exists("trained_models/trainedResnet.h5"):
        return "trainedResnet"
    else:
        return "pretrainedResnet"


def get_quant_model_name():
    if os.path.exists("trained_models/trainedResnet.h5"):
        return "trainedResnet"
    else:
        return "pretrainedResnet"


# ~200k params (at default conv_filters=26, input 32x32)
# input_shape and num_classes are now arguments — no longer hardcoded.
def resnet_v1_eembc(conv_filters=26, input_shape=(128, 128, 3), num_classes=6):

    num_filters = conv_filters

    inputs = Input(shape=input_shape)

    x = Conv2D(num_filters,
               kernel_size=3,
               strides=1,
               padding='same',
               kernel_initializer='he_normal',
               kernel_regularizer=l2(1e-4))(inputs)
    x = BatchNormalization()(x)
    x = Activation('relu')(x)

    # First stack
    y = Conv2D(num_filters,
               kernel_size=3,
               strides=1,
               padding='same',
               kernel_initializer='he_normal',
               kernel_regularizer=l2(1e-4))(x)
    y = BatchNormalization()(y)
    y = Activation('relu')(y)
    y = Conv2D(num_filters,
               kernel_size=3,
               strides=1,
               padding='same',
               kernel_initializer='he_normal',
               kernel_regularizer=l2(1e-4))(y)
    y = BatchNormalization()(y)
    x = tf.keras.layers.add([x, y])
    x = Activation('relu')(x)

    # Second stack
    num_filters = conv_filters * 2
    y = Conv2D(num_filters,
               kernel_size=3,
               strides=2,
               padding='same',
               kernel_initializer='he_normal',
               kernel_regularizer=l2(1e-4))(x)
    y = BatchNormalization()(y)
    y = Activation('relu')(y)
    y = Conv2D(num_filters,
               kernel_size=3,
               strides=1,
               padding='same',
               kernel_initializer='he_normal',
               kernel_regularizer=l2(1e-4))(y)
    y = BatchNormalization()(y)
    x = Conv2D(num_filters,
               kernel_size=1,
               strides=2,
               padding='same',
               kernel_initializer='he_normal',
               kernel_regularizer=l2(1e-4))(x)
    x = tf.keras.layers.add([x, y])
    x = Activation('relu')(x)

    # Third stack
    num_filters = conv_filters * 4
    y = Conv2D(num_filters,
               kernel_size=3,
               strides=2,
               padding='same',
               kernel_initializer='he_normal',
               kernel_regularizer=l2(1e-4))(x)
    y = BatchNormalization()(y)
    y = Activation('relu')(y)
    y = Conv2D(num_filters,
               kernel_size=3,
               strides=1,
               padding='same',
               kernel_initializer='he_normal',
               kernel_regularizer=l2(1e-4))(y)
    y = BatchNormalization()(y)
    x = Conv2D(num_filters,
               kernel_size=1,
               strides=2,
               padding='same',
               kernel_initializer='he_normal',
               kernel_regularizer=l2(1e-4))(x)
    x = tf.keras.layers.add([x, y])
    x = Activation('relu')(x)

    # Final classification layer.
    # pool_size is derived dynamically from the feature map, so this works
    # correctly for any input resolution (32x32, 128x128, etc.)

    pool_size = int(np.amin(x.shape[1:3]))
    x = AveragePooling2D(pool_size=pool_size)(x)
    y = Flatten()(x)
    outputs = Dense(num_classes,
                    activation='softmax',
                    kernel_initializer='he_normal')(y)

    model = Model(inputs=inputs, outputs=outputs)
    return model


# Lightweight CNN matching the architecture trained in Edge Impulse Studio's
# Neural Network designer for the finger digits classifier:
#     Conv2D(16, 3) -> MaxPool -> Conv2D(32, 3) -> MaxPool
#     -> Flatten -> Dropout(0.25) -> Dense(num_classes, softmax)
#
# Kept separate from resnet_v1_eembc so the two architectures can be trained
# and compared side by side through the same data/training/conversion
# pipeline (train.py, model_converter.py).
def ei_style_cnn(input_shape=(96, 96, 1), num_classes=7):

    inputs = Input(shape=input_shape)

    x = Conv2D(16, kernel_size=3, activation='relu')(inputs)
    x = MaxPooling2D()(x)

    x = Conv2D(32, kernel_size=3, activation='relu')(x)
    x = MaxPooling2D()(x)

    x = Flatten()(x)
    x = Dropout(0.25)(x)
    outputs = Dense(num_classes,
                    activation='softmax',
                    kernel_initializer='he_normal')(x)

    model = Model(inputs=inputs, outputs=outputs)
    return model
