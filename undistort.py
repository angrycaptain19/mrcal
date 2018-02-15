#!/usr/bin/python2

r'''Un-distorts image(s)

Synopsis:

  $ undistort.py --model left.cameramodel im1.png im2.png
  ========== CAMERAMODEL =========
  ... corresponding pinhole mrcal-native model
  ========== CAHVOR MODEL =========
  ... corresponding pinhole cahvor model
  Wrote im1_undistorted.png
  Wrote im2_undistorted.png

Given a single camera model (cahvor or mrcal-native) and some number of images,
this tool un-distorts each image and writes the result to disk. For each image
named xxxx.yyy, the new image filename is xxxx_undistorted.yyy. This tool
refuses to overwrite anything, and will barf if a target file already exists. A
corresponding pinhole camera model is also generated, and written to stdout.

Note that currently the corresponding pinhole model uses the same focal length,
center pixel values as the original, but no distortions. Thus the undistorted
images might cut out chunks of the original, or leave empty borders on the
edges.

A tool with similar usefulness is visualize-distortions.py. That tool renders a
vector field so that we can see the effect of a distortion model.

'''


import numpy as np
import numpysane as nps
import sys
import re
import cv2
import argparse
import os

from mrcal import cameramodel
from mrcal import cahvor
from mrcal import projections






def parse_args():

    parser = \
        argparse.ArgumentParser(description = __doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--model',
                        type=lambda f: f if os.path.isfile(f) else \
                                parser.error("The cameramodel must be an existing readable file, but got '{}'".format(f)),
                        required=True,
                        nargs=1,
                        help='''Input camera model. Assumed to be mrcal native, Unless the name is xxx.cahvor,
                        in which case the cahvor format is assumed''')

    parser.add_argument('image',
                        type=lambda f: f if os.path.isfile(f) else \
                                parser.error("The images must be readable files, but got '{}'".format(f)),
                        nargs='+',
                        help='''Images to undistort''')

    return parser.parse_args()

def target_image_filename(f):
    m = re.match("(.*)\.([a-z][a-z][a-z])$", f, flags=re.I)
    if not m:
        raise Exception("imagefile must end in .xxx where 'xxx' is some image extension. Instead got '{}'".format(imagefile))

    return "{}_undistorted.{}".format(m.group(1),m.group(2))


def writemodel(m):
    print("========== CAMERAMODEL =========")
    m.write(sys.stdout)
    print("")

    print("========== CAHVOR MODEL =========")
    cahvor.write(sys.stdout, m)



args = parse_args()




for imagefile in args.image:
    f = target_image_filename(imagefile)
    if os.path.isfile(f):
        raise Exception("Target image '{}' already exists. Giving up.".format(f))


if re.match(".*\.cahvor$", args.model[0]):
    model = cahvor.read(args.model[0])
else:
    model = cameramodel(args.model[0])

model_pinhole = cameramodel(model)
i = model_pinhole.intrinsics()
if i[0] == 'DISTORTION_NONE':
    sys.stderr.write("Warning: the mode has no distortions, so the 'undistorted' images will be the same as the input ones\n")
model_pinhole.intrinsics( ('DISTORTION_NONE',
                           i[1][:4] ) )

writemodel(model_pinhole)


for imagefile in args.image:
    imagefile_undistorted = target_image_filename(imagefile)

    image_undistorted     = projections.undistort_image(model, imagefile)
    cv2.imwrite(imagefile_undistorted, image_undistorted)
    sys.stderr.write("Wrote {}\n".format(imagefile_undistorted))
