#!/usr/bin/python2

import numpy as np
import numpysane as nps
import sys
import re
import cv2
import scipy.optimize

import utils
import mrcal



def _get_distortion_function(model):
    if "DISTORTION_CAHVOR"  == model:       return cahvor_distort
    if "DISTORTION_CAHVORE" == model:       return cahvore_distort
    if "DISTORTION_NONE"    == model:       return lambda p, *args: p
    if re.match("DISTORTION_OPENCV",model): return opencv_distort
    raise Exception("Unknown distortion model {}".format(model))


def cahvor_distort(p, fx, fy, cx, cy, *distortions):
    r'''Apply a CAHVOR warp to an un-distorted point

    Given intrinsic parameters of a CAHVOR model and a pinhole-projected
    point(s) numpy array of shape (..., 2), return the projected point(s) that
    we'd get with distortion. We ASSUME THE SAME fx,fy,cx,cy

    This function can broadcast the points array

    '''


    # this should go away in favor of a model-agnostic function that uses
    # mrcal.project()

    theta, phi, r0, r1, r2 = distortions

    # p is a 2d point. Convert to a 3d point
    p = nps.mv( nps.cat((p[..., 0] - cx)/fx,
                        (p[..., 1] - cy)/fy,
                        np.ones( p.shape[:-1])),
                0, -1 )
    o = np.array( (np.sin(phi) * np.cos(theta),
                   np.sin(phi) * np.sin(theta),
                   np.cos(phi) ))

    # cos( angle between p and o ) = inner(p,o) / (norm(o) * norm(p)) =
    # omega/norm(p)
    omega = nps.inner(p,o)

    # tau = 1/cos^2 - 1 = inner(p,p)/(omega*omega) - 1 =
    #     = tan^2
    tau   = nps.inner(p,p) / (omega*omega) - 1.0
    mu    = r0 + tau*r1 + tau*tau*r2
    p     = p*(nps.dummy(mu,-1)+1.0) - nps.dummy(mu*omega, -1)*o

    # now I apply a normal projection to the warped 3d point p
    return np.array((fx,fy)) * p[..., :2] / p[..., (2,)] + np.array((cx,cy))

@nps.broadcast_define( ((2,), (),(),(),(), (),(),(),(),(), (),(),(),(),),
                       (2,), )
def cahvore_distort(p, fx, fy, cx, cy, *distortions):
    r'''Apply a CAHVORE warp to an un-distorted point

    Given intrinsic parameters of a CAHVORE model and a pinhole-projected
    point(s) numpy array of shape (..., 2), return the projected point(s) that
    we'd get with distortion. We ASSUME THE SAME fx,fy,cx,cy

    This function has an implemented-in-python inner newton-raphson loop. AND
    this function broadcasts in python, so it is SLOW!

    '''

    # This comes from cmod_cahvore_3d_to_2d_general() in
    # m-jplv/libcmod/cmod_cahvore.c
    #
    # The lack of documentation heer comes directly from the lack of
    # documentation in that function.

    theta, phi, r0, r1, r2, e0, e1, e2, linearity = distortions

    # p is a 2d point. Convert to a 3d point
    p = nps.mv( nps.cat((p[..., 0] - cx)/fx,
                        (p[..., 1] - cy)/fy,
                        np.ones( p.shape[:-1])),
                0, -1 )
    o = np.array( (np.sin(phi) * np.cos(theta),
                   np.sin(phi) * np.sin(theta),
                   np.cos(phi) ))

    # cos( angle between p and o ) = inner(p,o) / (norm(o) * norm(p)) =
    # omega/norm(p)
    omega = nps.inner(p,o)



    # Basic Computations

    # Calculate initial terms
    u = omega * o
    l3 = p - u
    l  = np.sqrt(nps.inner(l3, l3))

    # Calculate theta using Newton's Method
    # FROM THIS POINT ON theta HAS A DIFFERENT MEANING THAN BEFORE
    theta = np.arctan2(l, omega)

    for inewton in xrange(100):
	# Compute terms from the current value of theta
	costh = np.cos(theta)
	sinth = np.sin(theta)
	theta2 = theta * theta
	theta3 = theta * theta2
	theta4 = theta * theta3
	upsilon = omega*costh + l*sinth \
		- (1     - costh) * (e0 +  e1*theta2 +   e2*theta4) \
		- (theta - sinth) * (      2*e1*theta  + 4*e2*theta3)

	# Update theta
	dtheta = ( \
		omega*sinth - l*costh \
		- (theta - sinth) * (e0 + e1*theta2 + e2*theta4) \
		) / upsilon
	theta -= dtheta

	# Check exit criterion from last update
	if abs(dtheta) < 1e-8:
	    break
    else:
        raise Exception("too many iterations")

    # got a theta

    # Check the value of theta
    if theta * abs(linearity) > np.pi/2.:
        raise Exception("theta out of bounds")

    # Approximations for small theta
    if theta < 1e-8:
        pass # p is good enough in this case

    # Full calculations
    else:
	linth = linearity * theta
	if linearity < -1e-15:
	    chi = np.sin(linth) / linearity
	elif linearity > 1e-15:
	    chi = np.tan(linth) / linearity
	else:
	    chi = theta

	chi2 = chi * chi
	chi3 = chi * chi2
	chi4 = chi * chi3

	zetap = l / chi

	mu = r0 + r1*chi2 + r2*chi4

        u  = zetap * o
        v  = (1. + mu)*l3
        p = u + v

    # now I apply a normal projection to the warped 3d point p
    return np.array((fx,fy)) * p[..., :2] / p[..., (2,)] + np.array((cx,cy))


def opencv_distort(p, fx, fy, cx, cy, *distortions):
    r'''Apply an OPENCV warp to an un-distorted point

    Given intrinsic parameters of an OPENCV model and a pinhole-projected
    point(s) numpy array of shape (..., 2), return the projected point(s) that
    we'd get with distortion. We ASSUME THE SAME fx,fy,cx,cy

    This function can broadcast the points array

    '''

    # this should go away in favor of a model-agnostic function that uses
    # mrcal.project()


    # opencv wants an Nx3 input array and an Nx2 output array. numpy
    # broadcasting rules allow any number of leading dimensions as long as
    # they're compatible. I squash the leading dimensions at the start, and put
    # them back when done


    dims_broadcast = p.shape[:-1]
    p = nps.clump(p, n=len(p.shape)-1)

    # p is a 2d point. Convert to a 3d point
    p = nps.mv( nps.cat((p[..., 0] - cx)/fx,
                        (p[..., 1] - cy)/fy,
                        np.ones( p.shape[:-1])),
                0, -1 )

    A = np.array(((fx,  0, cx),
                  ( 0, fy, cy),
                  ( 0,  0,  1)))

    out,_ = cv2.projectPoints(nps.atleast_dims(p,-2), np.zeros((3,)), np.zeros((3,)), A, distortions)
    out = out[:,0,:]

    out_dims = dims_broadcast + (2,)
    out = out.reshape(out_dims)
    return out

def _distort(p, distortion_model, fx, fy, cx, cy, *distortions):
    r'''Apply a distortion warp: distort a point

    This is a model-generic function. We use the given distortion_model: a
    string that says what the values in 'distortions' mean. The supported values
    are reported by mrcal.getSupportedDistortionModels(). At the time
    of this writing, the supported values are

      DISTORTION_NONE
      DISTORTION_OPENCV4
      DISTORTION_OPENCV5
      DISTORTION_OPENCV8
      DISTORTION_OPENCV12 (if we have OpenCV >= 3.0.0)
      DISTORTION_OPENCV14 (if we have OpenCV >= 3.1.0)
      DISTORTION_CAHVOR
      DISTORTION_CAHVORE

    Given intrinsic parameters of a model and a pinhole-projected point(s) numpy
    array of shape (..., 2), return the projected point(s) that we'd get with
    distortion. We ASSUME THE SAME fx,fy,cx,cy

    This function can broadcast the points array.

    '''


    # This now exists only for a CAHVORE path. Would be good to simplify, and
    # get rid of the flexibility

    if p is None or p.size == 0: return p


    Ndistortions = mrcal.getNdistortionParams(distortion_model)
    if len(distortions) != Ndistortions:
        raise Exception("Inconsistent distortion_model/values. Model '{}' expects {} distortion parameters, but got {} distortion values".format(distortion_model, Ndistortions, len(distortions)))

    if distortion_model == "DISTORTION_NONE" and not get_gradients:
        return p

    distort_function = _get_distortion_function(distortion_model)
    return distort_function(p, fx, fy, cx, cy, *distortions)

def _undistort(p, distortion_model, fx, fy, cx, cy, *distortions):
    r'''Un-apply a CAHVOR warp: undistort a point

    This is a model-generic function. We use the given distortion_model: a
    string that says what the values in 'distortions' mean. The supported values
    are defined in the DISTORTION_LIST macro in mrcal.h. At the time of this
    writing, the supported values are

      DISTORTION_NONE
      DISTORTION_OPENCV4
      DISTORTION_OPENCV5
      DISTORTION_OPENCV8
      DISTORTION_OPENCV12 (if we have OpenCV >= 3.0.0)
      DISTORTION_OPENCV14 (if we have OpenCV >= 3.1.0)
      DISTORTION_CAHVOR
      DISTORTION_CAHVORE

    Given intrinsic parameters of a model and a projected and distorted point(s)
    numpy array of shape (..., 2), return the projected point(s) that we'd get
    without distortion. We ASSUME THE SAME fx,fy,cx,cy

    This function can broadcast the points array.

    Note that this function has an iterative solver and is thus SLOW. This is
    the "backwards" direction. Most of the time you want distort().

    '''

    if p is None or p.size == 0: return p


    Ndistortions = mrcal.getNdistortionParams(distortion_model)
    if len(distortions) != Ndistortions:
        raise Exception("Inconsistent distortion_model/values. Model '{}' expects {} distortion parameters, but got {} distortion values", distortion_model, Ndistortions, len(distortions))

    if distortion_model == "DISTORTION_NONE":
        return p

    distort_function = _get_distortion_function(distortion_model)

    # I could make this much more efficient: precompute lots of stuff, use
    # gradients, etc, etc. This functions decently well and I leave it.

    # I optimize each point separately because the internal optimization
    # algorithm doesn't know that each point is independent, so if I optimized
    # it all together, it would solve a dense linear system whose size is linear
    # in Npoints. The computation time thus would be much slower than
    # linear(Npoints)
    @nps.broadcast_define( ((2,),), )
    def undistort_this(p0):
        def f(pundistorted):
            '''Optimization functions'''
            pdistorted = distort_function(pundistorted, fx,fy,cx,cy, *distortions)
            return pdistorted - p0
        p1 = scipy.optimize.leastsq(f, p0)[0]
        return np.array(p1)

    return undistort_this(p)

# @nps.broadcast_define( ((3,),('Nintrinsics',)),
#                        (2,), )
def project(p, intrinsics_or_distortionmodel, intrinsics=None, get_gradients=False):
    r'''Projects 3D point(s) using the given camera intrinsics

    Most of the time this invokes mrcal.project() directly UNLESS we're using
    CAHVORE. mrcal.project() does not support CAHVORE, so we implement our own
    path here. gradients are NOT implemented for CAHVORE

    This function is broadcastable over points only.

    Two interface types are supported:

    - project(p, distortion_model, intrinsics)

      Here 'intrinsics' are the parameters in a numpy array, so this invocation
      allows broadcasting over these intrinsics

    - project(p, intrinsics)

      Here intrinsics is a tuple (distortion_model, intrinsics_parameters), so
      you can do something like

        project(p, cameramodel("abc.cameramodel").intrinsics())

    Both invocations ingest three pieces of data:

    - p 3D point(s) in the camera coord system

    - distortion_model: a string that says what the values in the intrinsics
      array mean. The supported values are reported by
      mrcal.getSupportedDistortionModels(). At the time of this
      writing, the supported values are

        DISTORTION_NONE
        DISTORTION_OPENCV4
        DISTORTION_OPENCV5
        DISTORTION_OPENCV8
        DISTORTION_OPENCV12 (if we have OpenCV >= 3.0.0)
        DISTORTION_OPENCV14 (if we have OpenCV >= 3.1.0)
        DISTORTION_CAHVOR
        DISTORTION_CAHVORE

    - intrinsics: a numpy array containing
      - fx
      - fy
      - cx
      - cy
      - distortion-specific values

    if get_gradients: instead of returning the projected points I return a tuple

    - (...,2) array of projected pixel coordinates
    - (...,2,Nintrinsics) array of the gradients of the pixel coordinates in
      respect to the intrinsics
    - (...,2,3) array of the gradients of the pixel coordinates in respect to
      the input 3D point positions

    '''

    if intrinsics is None:
        distortion_model = intrinsics_or_distortionmodel[0]
        intrinsics_data  = intrinsics_or_distortionmodel[1]
    else:
        distortion_model = intrinsics_or_distortionmodel
        intrinsics_data  = intrinsics

    if p is None: return p
    if p.size == 0:
        Nintrinsics = intrinsics_data.shape[-1]
        if get_gradients:
            s = p.shape
            return np.zeros(s[:-1] + (2,)), np.zeros(s[:-1] + (2,Nintrinsics)), np.zeros(s[:-1] + (2,3))
        else:
            s = p.shape
            return np.zeros(s[:-1] + (2,))

    if distortion_model != 'DISTORTION_CAHVORE':
        return mrcal.project(np.ascontiguousarray(p),
                             distortion_model,
                             intrinsics_data,
                             get_gradients=get_gradients)

    # oof. CAHVORE. Lots of legacy code follows
    if get_gradients:
        raise Exception("Gradients not implemented for CAHVORE")

    def project_one_cam(intrinsics, p):
        fxy = intrinsics[:2]
        cxy = intrinsics[2:4]
        p2d = p[..., :2]/p[..., (2,)] * fxy + cxy
        return _distort(p2d, distortion_model, *intrinsics)

    # manually broadcast over intrinsics[]. The broadcast over p happens
    # implicitly.
    #
    # intrinsics shape I support: (a,b,c,..., Nintrinsics)
    # In my use case, at most one of a,b,c,... is != 1
    idims_not1 = [ i for i in xrange(len(intrinsics.shape)-1) if intrinsics.shape[i] != 1 ]
    if len(idims_not1) > 1:
        raise Exception("More than 1D worth of broadcasting for the intrinsics not implemented")

    if len(idims_not1) == 0:
        return project_one_cam(intrinsics.ravel(), p)

    idim_broadcast = idims_not1[0] - len(intrinsics.shape)
    Nbroadcast = intrinsics.shape[idim_broadcast]
    if p.shape[idim_broadcast] != Nbroadcast:
        raise Exception("Inconsistent dimensionality for broadcast at idim {}. p.shape: {} and intrinsics.shape: {}".format(idim_broadcast, p.shape, intrinsics.shape))

    psplit     = nps.mv(p,          idim_broadcast, 0)
    intrinsics = nps.mv(intrinsics, idim_broadcast, 0)

    return \
        nps.mv( nps.cat(*[ project_one_cam(intrinsics[i].ravel(),
                                           psplit[i])
                           for i in xrange(Nbroadcast)]),
                0, idim_broadcast )

def unproject(p, intrinsics_or_distortionmodel, intrinsics=None):
    r'''Computes unit vector(s) corresponding to pixel observation(s)

    This function is broadcastable over p (using numpy primitives intead of
    nps.broadcast_define() to avoid a slow python broadcasting loop).

    This function is NOT broadcastable over the intrinsics

    Two interface types are supported:

    - unproject(p, distortion_model, intrinsics)

      Here 'intrinsics' are the parameters in a numpy array, so this invocation
      allows broadcasting over these intrinsics

    - unproject(p, intrinsics)

      Here intrinsics is a tuple (distortion_model, intrinsics_parameters), so
      you can do something like

        unproject(p, cameramodel("abc.cameramodel").intrinsics())

    Both invocations ingest three pieces of data:

    - p 2D pixel coordinate(s)

    - distortion_model: a string that says what the values in the intrinsics
      array mean. The supported values are reported by
      mrcal.getSupportedDistortionModels(). At the time of this
      writing, the supported values are

        DISTORTION_NONE
        DISTORTION_OPENCV4
        DISTORTION_OPENCV5
        DISTORTION_OPENCV8
        DISTORTION_OPENCV12 (if we have OpenCV >= 3.0.0)
        DISTORTION_OPENCV14 (if we have OpenCV >= 3.1.0)
        DISTORTION_CAHVOR
        DISTORTION_CAHVORE

    - intrinsics: a numpy array containing
      - fx
      - fy
      - cx
      - cy
      - distortion-specific values

    '''


    if intrinsics is None:
        distortion_model = intrinsics_or_distortionmodel[0]
        intrinsics       = intrinsics_or_distortionmodel[1]
    else:
        distortion_model = intrinsics_or_distortionmodel

    if p is None: return p
    if p.size == 0:
        s = p.shape
        return np.zeros(s[:-1] + (3,))

    p = _undistort(p, distortion_model, *intrinsics)

    (fx, fy, cx, cy) = intrinsics[:4]

    # shape = (..., 2)
    P = (p - np.array((cx,cy))) / np.array((fx,fy))

    # I append a 1. shape = (..., 3)
    P = nps.glue(P, np.ones( P.shape[:-1] + (1,) ), axis=-1)

    # normalize each vector
    return P / nps.dummy(np.sqrt(nps.inner(P,P)), -1)

def distortion_map__to_warped(intrinsics, w, h):
    r'''Returns the pre and post distortion map of a model

    Takes in

    - a cahvor model (in a number of representations)
    - a sampling spacing on the x axis
    - a sampling spacing on the y axis

    Produces two arrays of shape Nwidth,Nheight,2:

    - grid: a meshgrid of the sampling spacings given as inputs. This refers to the
      UNDISTORTED image

    - dgrid: a meshgrid where each point in grid had the distortion corrected.
      This refers to the DISTORTED image

    '''

    # shape: Nwidth,Nheight,2
    grid  = nps.reorder(nps.cat(*np.meshgrid(w,h)), -1, -2, -3)

    distort_function = _get_distortion_function(intrinsics[0])
    dgrid = distort_function(grid, *intrinsics[1])
    return grid, dgrid

def undistort_image(model, image):
    r'''Visualize the distortion effect of a set of intrinsics

    This function warps an image to remove the distortion.

    An image is also input (could be a filename or an array). An array image is
    output.

    '''

    intrinsics = model.intrinsics()

    if not isinstance(image, np.ndarray):
        image = cv2.imread(image)

    H,W = image.shape[:2]
    _,mapxy = distortion_map__to_warped(intrinsics,
                                        np.arange(W), np.arange(H))
    mapx = mapxy[:,:,0].astype(np.float32)
    mapy = mapxy[:,:,1].astype(np.float32)

    remapped = cv2.remap(image,
                         nps.transpose(mapx),
                         nps.transpose(mapy),
                         cv2.INTER_LINEAR)
    return remapped

def calobservations_project(distortion_model, intrinsics, extrinsics, frames, dot_spacing, Nwant):
    r'''Takes in the same arguments as mrcal.optimize(), and returns all
    the projections. Output has shape (Nframes,Ncameras,Nwant,Nwant,2)

    '''

    object_ref = utils.get_ref_calibration_object(Nwant, Nwant, dot_spacing)
    Rf = utils.Rodrigues_toR_broadcasted(frames[:,:3])
    Rf = nps.mv(Rf,           0, -5)
    tf = nps.mv(frames[:,3:], 0, -5)

    # object in the cam0 coord system. shape=(Nframes, 1, Nwant, Nwant, 3)
    object_cam0 = nps.matmult( object_ref, nps.transpose(Rf)) + tf

    Rc = utils.Rodrigues_toR_broadcasted(extrinsics[:,:3])
    Rc = nps.mv(Rc,               0, -4)
    tc = nps.mv(extrinsics[:,3:], 0, -4)

    # object in the OTHER camera coord systems. shape=(Nframes, Ncameras-1, Nwant, Nwant, 3)
    object_cam_others = nps.matmult( object_cam0, nps.transpose(Rc)) + tc

    # object in the ALL camera coord systems. shape=(Nframes, Ncameras, Nwant, Nwant, 3)
    object_cam = nps.glue(object_cam0, object_cam_others, axis=-4)

    # projected points. shape=(Nframes, Ncameras, Nwant, Nwant, 2)

    # loop over Ncameras. project() will broadcast over the points
    intrinsics = nps.atleast_dims(intrinsics, -2)
    Ncameras = intrinsics.shape[-2]
    return nps.mv( nps.cat(*[project( object_cam[...,i_camera,:,:,:],
                                      distortion_model,
                                      intrinsics[i_camera,:] ) for \
                             i_camera in xrange(Ncameras)]),
                   0,-4)

def calobservations_compute_reproj_error(projected, observations, indices_frame_camera, Nwant,
                                         outlier_indices = np.array(())):
    r'''Computes reprojection errors when calibrating with board observations

    Given

    - projected (shape [Nframes,Ncameras,Nwant,Nwant,2])
    - observations (shape [Nframes,Nwant,Nwant,2])
    - indices_frame_camera (shape [Nobservations,2])
    - outlier_indices, a list of point indices that were deemed to be outliers.
      These are plain integers indexing the flattened observations array, but
      one per POINT, not (x,y) independently

    Return (err_all_points,err_ignoring_outliers). Each is the reprojection
    error for each point: shape [Nobservations,Nwant,Nwant,2]. One includes the
    outliers in the returned errors, and the other does not

    '''

    Nframes               = projected.shape[0]
    Nobservations         = indices_frame_camera.shape[0]
    err_all_points        = np.zeros((Nobservations,Nwant,Nwant,2))

    for i_observation in xrange(Nobservations):
        i_frame, i_camera = indices_frame_camera[i_observation]

        err_all_points[i_observation] = projected[i_frame,i_camera] - observations[i_observation]

    err_ignoring_outliers = err_all_points.copy()
    err_ignoring_outliers.ravel()[outlier_indices*2  ] = 0
    err_ignoring_outliers.ravel()[outlier_indices*2+1] = 0

    return err_all_points,err_ignoring_outliers

