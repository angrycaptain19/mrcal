#!/usr/bin/python3

r'''Tests projections, unprojections, distortions, etc'''

import sys
import numpy as np
import numpysane as nps
import os

testdir = os.path.dirname(os.path.realpath(__file__))

# I import the LOCAL mrcal since that's what I'm testing
sys.path[:0] = f"{testdir}/..",
import mrcal
import testutils


m               = mrcal.cameramodel(f"{testdir}/data/opencv8.cameramodel")
W,H             = m.imagersize()
intrinsics_core = m.intrinsics()[1][:4]

# testutils.confirm_equal( mrcal.compute_scale_f_pinhole_for_fit(m, None),
#                          1.0,
#                          msg = 'compute_scale_f_pinhole_for_fit')


def fit_check(scale_f_pinhole, intrinsics, v,
              W                        = W,
              H                        = H,
              scale_imagersize_pinhole = 1.0,
              eps                      = 1e-2):
    r'''Makes sure projected vectors fit into the imager perfectly

    I'm given a number of points in the camera coords. These much project such
    that

    - All projected points lie INSIDE the imager
    - At least one point lies exactly on the imager boundary

    '''
    intrinsics = intrinsics.copy()
    intrinsics[:2] *= scale_f_pinhole
    intrinsics     *= scale_imagersize_pinhole

    q = mrcal.project(v, 'DISTORTION_NONE', intrinsics)

    if any( q[:,0] < -eps )    or \
       any( q[:,0] > W-1+eps ) or \
       any( q[:,1] < -eps )    or \
       any( q[:,1] > H-1+eps ):
        return ": Some points lie out of bounds"

    on_left_edge   = np.abs(q[:,0]        ) < eps
    on_right_edge  = np.abs(q[:,0] - (W-1)) < eps
    on_top_edge    = np.abs(q[:,1]        ) < eps
    on_bottom_edge = np.abs(q[:,1] - (H-1)) < eps

    if not ( \
             any(on_left_edge)  or \
             any(on_top_edge)   or \
             any(on_right_edge) or \
             any(on_bottom_edge) ):
        return ": No points lie on the edge"

    # all good
    return ''


err_msg = \
    fit_check( mrcal.compute_scale_f_pinhole_for_fit(m, 'corners'),
               intrinsics_core,
               mrcal.unproject( np.array(((  0 ,  0),
                                          (W-1,   0),
                                          (  0, H-1),
                                          (W-1, H-1)), dtype=float),
                                *m.intrinsics()),)
testutils.confirm( err_msg == '',
                   msg = 'compute_scale_f_pinhole_for_fit' + err_msg)

err_msg = \
    fit_check( mrcal.compute_scale_f_pinhole_for_fit(m, 'centers-horizontal'),
               intrinsics_core,
               mrcal.unproject( np.array(((  0, (H-1.)/2.),
                                          (W-1, (H-1.)/2.)), dtype=float),
                                *m.intrinsics()),)
testutils.confirm( err_msg == '',
                   msg = 'compute_scale_f_pinhole_for_fit' + err_msg)

err_msg = \
    fit_check( mrcal.compute_scale_f_pinhole_for_fit(m, 'centers-vertical'),
               intrinsics_core,
               mrcal.unproject( np.array((((W-1.)/2.,   0.),
                                          ((W-1.)/2., H-1.)), dtype=float),
                                *m.intrinsics()),)
testutils.confirm( err_msg == '',
                   msg = 'compute_scale_f_pinhole_for_fit' + err_msg)

err_msg = \
    fit_check( mrcal.compute_scale_f_pinhole_for_fit(m,
                                                     np.array((((W-1.)/2.,   0.),
                                                               ((W-1.)/2., H-1.)), dtype=float)),
               intrinsics_core,
               mrcal.unproject( np.array((((W-1.)/2.,   0.),
                                          ((W-1.)/2., H-1.)), dtype=float),
                                *m.intrinsics()),)
testutils.confirm( err_msg == '',
                   msg = 'compute_scale_f_pinhole_for_fit' + err_msg)

err_msg = \
    fit_check( mrcal.compute_scale_f_pinhole_for_fit(m, 'centers-horizontal',
                                                     scale_imagersize_pinhole = 0.5),
               intrinsics_core,
               mrcal.unproject( np.array(((  0, (H-1.)/2.),
                                          (W-1, (H-1.)/2.)), dtype=float),
                                *m.intrinsics()),
               W = W / 2.,
               H = H / 2.,
               scale_imagersize_pinhole = 0.5)
testutils.confirm( err_msg == '',
                   msg = 'compute_scale_f_pinhole_for_fit' + err_msg)


testutils.finish()