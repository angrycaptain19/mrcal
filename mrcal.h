#pragma once

#include <stdbool.h>

#include "basic_points.h"







// unconstrained 6DOF pose containing a rodrigues rotation and a translation
struct pose_t
{
    union point3_t r,t;
};

// An observation of a calibration board. Each "observation" is ONE camera
// observing a board
struct observation_board_t
{
    int  i_camera         : 31;
    bool skip_frame       : 1;
    int  i_frame          : 31;
    bool skip_observation : 1;

    union point2_t* px; // NUM_POINTS_IN_CALOBJECT of these
};

struct observation_point_t
{
    int  i_camera         : 31;
    bool skip_point       : 1;
    int  i_point          : 31;
    bool skip_observation : 1;

    // Observed pixel coordinates
    union point2_t px;

    // Reference distance. This is optional; skipped if <= 0
    double dist;
};



struct intrinsics_core_t
{
    double focal_xy [2];
    double center_xy[2];
};
#define N_INTRINSICS_CORE ((int)(sizeof(struct intrinsics_core_t)/sizeof(double)))


// names of distortion models, number of distortion parameters
#define DISTORTION_LIST(_)                      \
    _(DISTORTION_NONE,    0)                    \
    _(DISTORTION_OPENCV4, 4)                    \
    _(DISTORTION_OPENCV5, 5)                    \
    _(DISTORTION_OPENCV8, 8)                    \
    _(DISTORTION_OPENCV12,12) /* available in OpenCV >= 3.0.0) */ \
    _(DISTORTION_OPENCV14,14) /* available in OpenCV >= 3.1.0) */ \
    _(DISTORTION_CAHVOR,  5)                    \
    _(DISTORTION_CAHVORE, 9) /* CAHVORE is CAHVOR + E + linearity */

#define LIST_WITH_COMMA(s,n) ,s
enum distortion_model_t
    { DISTORTION_INVALID DISTORTION_LIST( LIST_WITH_COMMA ) };


struct mrcal_variable_select
{
    bool do_optimize_intrinsic_core        : 1;
    bool do_optimize_intrinsic_distortions : 1;
    bool do_optimize_extrinsics            : 1;
    bool do_optimize_frames                : 1;
    bool do_skip_regularization            : 1;
};
#define DO_OPTIMIZE_ALL ((struct mrcal_variable_select) { .do_optimize_intrinsic_core        = true, \
                                                          .do_optimize_intrinsic_distortions = true, \
                                                          .do_optimize_extrinsics            = true, \
                                                          .do_optimize_frames                = true})
#define IS_OPTIMIZE_NONE(x)                     \
    (!(x).do_optimize_intrinsic_core &&         \
     !(x).do_optimize_intrinsic_distortions &&  \
     !(x).do_optimize_extrinsics &&             \
     !(x).do_optimize_frames)


const char*             mrcal_distortion_model_name       ( enum distortion_model_t model );
enum distortion_model_t mrcal_distortion_model_from_name  ( const char* name );
int                     mrcal_getNdistortionParams        ( const enum distortion_model_t m );
int                     mrcal_getNintrinsicParams         ( const enum distortion_model_t m );
int                     mrcal_getNintrinsicOptimizationParams
                          ( struct mrcal_variable_select optimization_variable_choice,
                            enum distortion_model_t m );
const char* const*      mrcal_getSupportedDistortionModels( void ); // NULL-terminated array of char* strings

void mrcal_project( // out
                   union point2_t* out,

                   // core, distortions concatenated. Stored as a row-first
                   // array of shape (N,2,Nintrinsics)
                   double*         dxy_dintrinsics,
                   // Stored as a row-first array of shape (N,2). Each element
                   // of this array is a point3_t
                   union point3_t* dxy_dp,

                   // in
                   const union point3_t* p,
                   int N,
                   enum distortion_model_t distortion_model,
                   // core, distortions concatenated
                   const double* intrinsics);



#define MRCAL_STATS_ITEM(_)                                           \
    _(double,         rms_reproj_error__pixels,   PyFloat_FromDouble) \
    _(int,            Noutliers,                  PyLong_FromLong)

#define MRCAL_STATS_ITEM_DEFINE(type, name, pyconverter) type name;

struct mrcal_stats_t
{
    MRCAL_STATS_ITEM(MRCAL_STATS_ITEM_DEFINE)
};

struct mrcal_stats_t
mrcal_optimize( // out
                // These may be NULL. They're for diagnostic reporting to the
                // caller
                double* x_final,
                double* intrinsic_covariances,
                // Buffer should be at least Npoints long. stats->Noutliers
                // elements will be filled in
                int*    outlier_indices_final,

                // out, in

                // if(_solver_context != NULL) then this is a persistent solver
                // context. The context is NOT freed on exit.
                // mrcal_free_context() should be called to release it
                //
                // if(*_solver_context != NULL), the given context is reused
                // if(*_solver_context == NULL), a context is created, and
                // returned here on exit
                void** _solver_context,

                // These are a seed on input, solution on output
                // These are the state. I don't have a state_t because Ncameras
                // and Nframes aren't known at compile time.
                //
                // camera_intrinsics is a concatenation of the intrinsics
                // core and the distortion params. The specific distortion
                // parameters may vary, depending on distortion_model, so
                // this is a variable-length structure
                double*              camera_intrinsics,  // Ncameras * (N_INTRINSICS_CORE + Ndistortions)
                struct pose_t*       camera_extrinsics,  // Ncameras-1 of these. Transform FROM camera0 frame
                struct pose_t*       frames,             // Nframes of these.    Transform TO   camera0 frame
                union  point3_t*     points,             // Npoints of these.    In the camera0 frame

                // in
                int Ncameras, int Nframes, int Npoints,

                const struct observation_board_t* observations_board,
                int NobservationsBoard,

                const struct observation_point_t* observations_point,
                int NobservationsPoint,

                bool check_gradient,
                bool VERBOSE,
                const bool skip_outlier_rejection,

                enum distortion_model_t distortion_model,
                struct mrcal_variable_select optimization_variable_choice,

                double calibration_object_spacing,
                int calibration_object_width_n);

int mrcal_getNmeasurements(int Ncameras, int NobservationsBoard,
                           const struct observation_point_t* observations_point,
                           int NobservationsPoint,
                           int calibration_object_width_n,
                           struct mrcal_variable_select optimization_variable_choice,
                           enum distortion_model_t distortion_model);

// Given a set of 3d points, returns the expected-value of the outlierness
// factor, for each, if it was added to the data set with a nominal distribution
// on the inputs; the caller needs to adjust this to the REAL distribution
bool mrcal_queryIntrinsicOutliernessAt( // output
                                       double* traces,

                                       // input
                                       enum distortion_model_t distortion_model,
                                       bool do_optimize_intrinsic_core,
                                       bool do_optimize_intrinsic_distortions,
                                       int i_camera,

                                       // query vectors (and a count) in the
                                       // camera coord system. We're
                                       // projecting these
                                       const union point3_t* v,
                                       int N,

                                       int Noutliers,

                                       // context from the solve we just ran.
                                       // I need this for the factorized JtJ
                                       void* _solver_context);

void mrcal_free_context(void** ctx);
