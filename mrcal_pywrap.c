#define NPY_NO_DEPRECATED_API NPY_API_VERSION

#include <stdbool.h>
#include <Python.h>
#include <structmember.h>
#include <numpy/arrayobject.h>
#include <signal.h>

#include "mrcal.h"


// Python is silly. There's some nuance about signal handling where it sets a
// SIGINT (ctrl-c) handler to just set a flag, and the python layer then reads
// this flag and does the thing. Here I'm running C code, so SIGINT would set a
// flag, but not quit, so I can't interrupt the solver. Thus I reset the SIGINT
// handler to the default, and put it back to the python-specific version when
// I'm done
#define SET_SIGINT() struct sigaction sigaction_old;                    \
do {                                                                    \
    if( 0 != sigaction(SIGINT,                                          \
                       &(struct sigaction){ .sa_handler = SIG_DFL },    \
                       &sigaction_old) )                                \
    {                                                                   \
        PyErr_SetString(PyExc_RuntimeError, "sigaction() failed");      \
        goto done;                                                      \
    }                                                                   \
} while(0)
#define RESET_SIGINT() do {                                             \
    if( 0 != sigaction(SIGINT,                                          \
                       &sigaction_old, NULL ))                          \
        PyErr_SetString(PyExc_RuntimeError, "sigaction-restore failed"); \
} while(0)

#define QUOTED_LIST_WITH_COMMA(s,n) "'" #s "',"

#define CHECK_CONTIGUOUS(x) do {                                        \
    if( !PyArray_IS_C_CONTIGUOUS(x) )                                   \
    {                                                                   \
        PyErr_SetString(PyExc_RuntimeError, "All inputs must be c-style contiguous arrays (" #x ")"); \
        return false;                                                   \
    } } while(0)


#define COMMA ,
#define ARG_DEFINE(     name, pytype, initialvalue, parsecode, parseprearg, npy_type, dims_ref) pytype name = initialvalue;
#define ARG_LIST_DEFINE(name, pytype, initialvalue, parsecode, parseprearg, npy_type, dims_ref) pytype name,
#define ARG_LIST_CALL(  name, pytype, initialvalue, parsecode, parseprearg, npy_type, dims_ref) name,
#define NAMELIST(       name, pytype, initialvalue, parsecode, parseprearg, npy_type, dims_ref) #name ,
#define PARSECODE(      name, pytype, initialvalue, parsecode, parseprearg, npy_type, dims_ref) parsecode
#define PARSEARG(       name, pytype, initialvalue, parsecode, parseprearg, npy_type, dims_ref) parseprearg &name,
#define FREE_PYARRAY(   name, pytype, initialvalue, parsecode, parseprearg, npy_type, dims_ref) if((int)npy_type >= 0 && name) { Py_DECREF(name); }
#define CHECK_LAYOUT(   name, pytype, initialvalue, parsecode, parseprearg, npy_type, dims_ref) \
    if((int)npy_type >= 0 && (PyObject*)name != NULL && (PyObject*)name != (PyObject*)Py_None) { \
        int dims[] = dims_ref;                                          \
        int ndims = (int)sizeof(dims)/(int)sizeof(dims[0]);             \
                                                                        \
        if( ndims > 0 )                                                 \
        {                                                               \
            if( PyArray_NDIM((PyArrayObject*)name) != ndims )           \
            {                                                           \
                PyErr_Format(PyExc_RuntimeError, "'" #name "' must have exactly %d dims; got %d", ndims, PyArray_NDIM((PyArrayObject*)name)); \
                return false;                                           \
            }                                                           \
            for(int i=0; i<ndims; i++)                                  \
                if(dims[i] >= 0 && dims[i] != PyArray_DIMS((PyArrayObject*)name)[i]) \
                {                                                       \
                    PyErr_Format(PyExc_RuntimeError, "'" #name "'must have dimensions '" #dims_ref "' where <0 means 'any'. Dims %d got %ld instead", i, PyArray_DIMS((PyArrayObject*)name)[i]); \
                    return false;                                       \
                }                                                       \
        }                                                               \
        if( (int)npy_type >= 0 )                                        \
        {                                                               \
            if( PyArray_TYPE((PyArrayObject*)name) != npy_type )        \
            {                                                           \
                PyErr_SetString(PyExc_RuntimeError, "'" #name "' must have type: " #npy_type); \
                return false;                                           \
            }                                                           \
            if( !PyArray_IS_C_CONTIGUOUS((PyArrayObject*)name) )        \
            {                                                           \
                PyErr_SetString(PyExc_RuntimeError, "'" #name "'must be c-style contiguous"); \
                return false;                                           \
            }                                                           \
        }                                                               \
    }


// Silly wrapper around a solver context and various solver metadata. I need the
// optimization to be able to keep this, and I need Python to free it as
// necessary when the refcount drops to 0
typedef struct {
    PyObject_HEAD
    void* ctx;
    enum distortion_model_t distortion_model;
    bool do_optimize_intrinsic_core;
    bool do_optimize_intrinsic_distortions;
    bool cahvor_radial_only;
} SolverContext;
static void SolverContext_free(SolverContext* self)
{
    mrcal_free_context(&self->ctx);
    Py_TYPE(self)->tp_free((PyObject*)self);
}
static PyObject* SolverContext_str(SolverContext* self)
{
    if(self->ctx == NULL)
        return PyString_FromString("Empty context");
    return PyString_FromFormat("Non-empty context made with        %s\n"
                               "do_optimize_intrinsic_core:        %d\n"
                               "do_optimize_intrinsic_distortions: %d\n"
                               "cahvor_radial_only:                %d\n",
                               mrcal_distortion_model_name(self->distortion_model),
                               self->do_optimize_intrinsic_core,
                               self->do_optimize_intrinsic_distortions,
                               self->cahvor_radial_only);
}
static PyTypeObject SolverContextType =
{
     PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "mrcal.SolverContext",
    .tp_basicsize = sizeof(SolverContext),
    .tp_new       = PyType_GenericNew,
    .tp_dealloc   = (destructor)SolverContext_free,
    .tp_str       = (reprfunc)SolverContext_str,
    .tp_repr      = (reprfunc)SolverContext_str,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "Opaque solver context used by mrcal",
};


static PyObject* getNdistortionParams(PyObject* NPY_UNUSED(self),
                                      PyObject* args)
{
    PyObject* result = NULL;
    SET_SIGINT();

    PyObject* distortion_model_string = NULL;
    if(!PyArg_ParseTuple( args, "S", &distortion_model_string ))
        goto done;

    const char* distortion_model_cstring =
        PyString_AsString(distortion_model_string);
    if( distortion_model_cstring == NULL)
    {
        PyErr_SetString(PyExc_RuntimeError, "Distortion model was not passed in. Must be a string, one of ("
                        DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                        ")");
        goto done;
    }

    enum distortion_model_t distortion_model = mrcal_distortion_model_from_name(distortion_model_cstring);
    if( distortion_model == DISTORTION_INVALID )
    {
        PyErr_Format(PyExc_RuntimeError, "Invalid distortion model was passed in: '%s'. Must be a string, one of ("
                     DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                     ")",
                     distortion_model_cstring);
        goto done;
    }

    int Ndistortions = mrcal_getNdistortionParams(distortion_model);

    result = Py_BuildValue("i", Ndistortions);

 done:
    RESET_SIGINT();
    return result;
}

static PyObject* getSupportedDistortionModels(PyObject* NPY_UNUSED(self),
                                              PyObject* NPY_UNUSED(args))
{
    PyObject* result = NULL;
    SET_SIGINT();
    const char* const* names = mrcal_getSupportedDistortionModels();

    // I now have a NULL-terminated list of NULL-terminated strings. Get N
    int N=0;
    while(names[N] != NULL)
        N++;

    result = PyTuple_New(N);
    if(result == NULL)
    {
        PyErr_Format(PyExc_RuntimeError, "Failed PyTuple_New(%d)", N);
        goto done;
    }

    for(int i=0; i<N; i++)
    {
        PyObject* name = Py_BuildValue("s", names[i]);
        if( name == NULL )
        {
            PyErr_Format(PyExc_RuntimeError, "Failed Py_BuildValue...");
            Py_DECREF(result);
            result = NULL;
            goto done;
        }
        PyTuple_SET_ITEM(result, i, name);
    }

 done:
    RESET_SIGINT();
    return result;
}

static PyObject* getNextDistortionModel(PyObject* NPY_UNUSED(self),
                                        PyObject* args)
{
    PyObject* result = NULL;
    SET_SIGINT();

    PyObject* distortion_model_now_string   = NULL;
    PyObject* distortion_model_final_string = NULL;
    if(!PyArg_ParseTuple( args, "SS",
                          &distortion_model_now_string,
                          &distortion_model_final_string))
        goto done;

    const char* distortion_model_now_cstring = PyString_AsString(distortion_model_now_string);
    if( distortion_model_now_cstring == NULL)
    {
        PyErr_SetString(PyExc_RuntimeError, "distortion_model_now was not passed in. Must be a string, one of ("
                        DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                        ")");
        goto done;
    }
    const char* distortion_model_final_cstring = PyString_AsString(distortion_model_final_string);
    if( distortion_model_final_cstring == NULL)
    {
        PyErr_SetString(PyExc_RuntimeError, "distortion_model_final was not passed in. Must be a string, one of ("
                        DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                        ")");
        goto done;
    }

    enum distortion_model_t distortion_model_now = mrcal_distortion_model_from_name(distortion_model_now_cstring);
    if( distortion_model_now == DISTORTION_INVALID )
    {
        PyErr_Format(PyExc_RuntimeError, "Invalid distortion_model_now was passed in: '%s'. Must be a string, one of ("
                     DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                     ")",
                     distortion_model_now_cstring);
        goto done;
    }
    enum distortion_model_t distortion_model_final = mrcal_distortion_model_from_name(distortion_model_final_cstring);
    if( distortion_model_final == DISTORTION_INVALID )
    {
        PyErr_Format(PyExc_RuntimeError, "Invalid distortion_model_final was passed in: '%s'. Must be a string, one of ("
                     DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                     ")",
                     distortion_model_final_cstring);
        goto done;
    }

    enum distortion_model_t distortion_model =
        mrcal_getNextDistortionModel(distortion_model_now, distortion_model_final);
    if(distortion_model == DISTORTION_INVALID)
    {
        PyErr_Format(PyExc_RuntimeError, "Couldn't figure out the 'next' distortion model from '%s' to '%s'",
                     distortion_model_now_cstring, distortion_model_final_cstring);
        goto done;
    }

    result = Py_BuildValue("s", mrcal_distortion_model_name(distortion_model));

 done:
    RESET_SIGINT();
    return result;
}

// just like PyArray_Converter(), but leave None as None
static
int PyArray_Converter_leaveNone(PyObject* obj, PyObject** address)
{
    if(obj == Py_None)
    {
        *address = Py_None;
        Py_INCREF(Py_None);
        return 1;
    }
    return PyArray_Converter(obj,address);
}

#define PROJECT_ARGUMENTS_REQUIRED(_)                                  \
    _(points,           PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {} ) \
    _(distortion_model, PyObject*,      NULL,    "S",                                   , -1,         {} ) \
    _(intrinsics,       PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {} ) \

#define PROJECT_ARGUMENTS_OPTIONAL(_) \
    _(get_gradients,    PyObject*,  Py_False,    "O",                                   ,         -1, {})

#define PROJECT_ARGUMENTS_ALL(_) \
    PROJECT_ARGUMENTS_REQUIRED(_) \
    PROJECT_ARGUMENTS_OPTIONAL(_)
static bool project_validate_args( // out
                                  enum distortion_model_t* distortion_model_type,

                                  // in
                                  PROJECT_ARGUMENTS_REQUIRED(ARG_LIST_DEFINE)
                                  PROJECT_ARGUMENTS_OPTIONAL(ARG_LIST_DEFINE)
                                  void* dummy __attribute__((unused)))
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    PROJECT_ARGUMENTS_ALL(CHECK_LAYOUT) ;
#pragma GCC diagnostic pop


    if( PyArray_NDIM(intrinsics) != 1 )
    {
        PyErr_SetString(PyExc_RuntimeError, "'intrinsics' must have exactly 1 dim");
        return false;
    }

    if( PyArray_NDIM(points) < 1 )
    {
        PyErr_SetString(PyExc_RuntimeError, "'points' must have ndims >= 1");
        return false;
    }
    if( 3 != PyArray_DIMS(points)[ PyArray_NDIM(points)-1 ] )
    {
        PyErr_Format(PyExc_RuntimeError, "points.shape[-1] MUST be 3. Instead got %ld",
                     PyArray_DIMS(points)[PyArray_NDIM(points)-1] );
        return false;
    }

    const char* distortion_model_cstring = PyString_AsString(distortion_model);
    if( distortion_model_cstring == NULL)
    {
        PyErr_SetString(PyExc_RuntimeError, "Distortion model was not passed in. Must be a string, one of ("
                        DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                        ")");
        return false;
    }

    *distortion_model_type = mrcal_distortion_model_from_name(distortion_model_cstring);
    if( *distortion_model_type == DISTORTION_INVALID )
    {
        PyErr_Format(PyExc_RuntimeError, "Invalid distortion model was passed in: '%s'. Must be a string, one of ("
                     DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                     ")",
                     distortion_model_cstring);
        return false;
    }

    int NdistortionParams = mrcal_getNdistortionParams(*distortion_model_type);
    if( N_INTRINSICS_CORE + NdistortionParams != PyArray_DIMS(intrinsics)[0] )
    {
        PyErr_Format(PyExc_RuntimeError, "intrinsics.shape[1] MUST be %d. Instead got %ld",
                     N_INTRINSICS_CORE + NdistortionParams,
                     PyArray_DIMS(intrinsics)[1] );
        return false;
    }

    return true;
}
static PyObject* project(PyObject* NPY_UNUSED(self),
                         PyObject* args,
                         PyObject* kwargs)
{
    PyObject*      result          = NULL;
    SET_SIGINT();

    PyArrayObject* out             = NULL;
    PyArrayObject* dxy_dintrinsics = NULL;
    PyArrayObject* dxy_dp          = NULL;

    PROJECT_ARGUMENTS_ALL(ARG_DEFINE) ;
    char* keywords[] = { PROJECT_ARGUMENTS_REQUIRED(NAMELIST)
                         PROJECT_ARGUMENTS_OPTIONAL(NAMELIST)
                         NULL};

    if(!PyArg_ParseTupleAndKeywords( args, kwargs,
                                     PROJECT_ARGUMENTS_REQUIRED(PARSECODE) "|"
                                     PROJECT_ARGUMENTS_OPTIONAL(PARSECODE),

                                     keywords,

                                     PROJECT_ARGUMENTS_REQUIRED(PARSEARG)
                                     PROJECT_ARGUMENTS_OPTIONAL(PARSEARG) NULL))
        goto done;

    enum distortion_model_t distortion_model_type;
    if(!project_validate_args( &distortion_model_type,
                               PROJECT_ARGUMENTS_REQUIRED(ARG_LIST_CALL)
                               PROJECT_ARGUMENTS_OPTIONAL(ARG_LIST_CALL)
                               NULL))
        goto done;

    int Nintrinsics = PyArray_DIMS(intrinsics)[0];

    // poor man's broadcasting of the inputs. I compute the total number of
    // points by multiplying the extra broadcasted dimensions. And I set up the
    // outputs to have the appropriate broadcasted dimensions
    const npy_intp* leading_dims  = PyArray_DIMS(points);
    int             Nleading_dims = PyArray_NDIM(points)-1;
    int Npoints = 1;
    for(int i=0; i<Nleading_dims; i++)
        Npoints *= leading_dims[i];
    {
        npy_intp dims[Nleading_dims+2];
        memcpy(dims, leading_dims, Nleading_dims*sizeof(dims[0]));

        dims[Nleading_dims + 0] = 2;
        out = (PyArrayObject*)PyArray_SimpleNew(Nleading_dims+1,
                                                dims,
                                                NPY_DOUBLE);
        if( get_gradients )
        {
            dims[Nleading_dims + 0] = 2;
            dims[Nleading_dims + 1] = Nintrinsics;
            dxy_dintrinsics = (PyArrayObject*)PyArray_SimpleNew(Nleading_dims+2,
                                                                dims,
                                                                NPY_DOUBLE);

            dims[Nleading_dims + 0] = 2;
            dims[Nleading_dims + 1] = 3;
            dxy_dp          = (PyArrayObject*)PyArray_SimpleNew(Nleading_dims+2,
                                                                dims,
                                                                NPY_DOUBLE);
        }
    }

    mrcal_project((union point2_t*)PyArray_DATA(out),
                  get_gradients ? (double*)PyArray_DATA(dxy_dintrinsics) : NULL,
                  get_gradients ? (union point3_t*)PyArray_DATA(dxy_dp)  : NULL,

                  (const union point3_t*)PyArray_DATA(points),
                  Npoints,
                  distortion_model_type,
                  // core, distortions concatenated
                  (const double*)PyArray_DATA(intrinsics));

    if( PyObject_IsTrue(get_gradients) )
    {
        result = PyTuple_New(3);
        PyTuple_SET_ITEM(result, 0, (PyObject*)out);
        PyTuple_SET_ITEM(result, 1, (PyObject*)dxy_dintrinsics);
        PyTuple_SET_ITEM(result, 2, (PyObject*)dxy_dp);
    }
    else
        result = (PyObject*)out;

 done:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    PROJECT_ARGUMENTS_REQUIRED(FREE_PYARRAY) ;
    PROJECT_ARGUMENTS_OPTIONAL(FREE_PYARRAY) ;
#pragma GCC diagnostic pop
    RESET_SIGINT();
    return result;
}



#define QIOA_ARGUMENTS_REQUIRED(_)                                  \
    _(v,              PyArrayObject*, NULL, "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {}) \
    _(i_camera,       int,            -1,   "i",                                   ,         -1, {}) \
    _(solver_context, SolverContext*, NULL, "O" ,                                  ,         -1, {})

#define QIOA_ARGUMENTS_OPTIONAL(_) \
    _(Noutliers,      int,            0,    "i",                                   ,         -1, {})

#define QIOA_ARGUMENTS_ALL(_) \
    QIOA_ARGUMENTS_REQUIRED(_) \
    QIOA_ARGUMENTS_OPTIONAL(_)
static
bool queryIntrinsicOutliernessAt_validate_args(QIOA_ARGUMENTS_REQUIRED(ARG_LIST_DEFINE)
                                               QIOA_ARGUMENTS_OPTIONAL(ARG_LIST_DEFINE)
                                               void* dummy __attribute__((unused)))
{
    if( PyArray_NDIM(v) < 1 )
    {
        PyErr_SetString(PyExc_RuntimeError, "'v' must have ndims >= 1");
        return false;
    }
    if( 3 != PyArray_DIMS(v)[ PyArray_NDIM(v)-1 ] )
    {
        PyErr_Format(PyExc_RuntimeError, "v.shape[-1] MUST be 3. Instead got %ld",
                     PyArray_DIMS(v)[PyArray_NDIM(v)-1] );
        return false;
    }


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    QIOA_ARGUMENTS_ALL(CHECK_LAYOUT) ;
#pragma GCC diagnostic pop

    if(i_camera < 0)
    {
        PyErr_Format(PyExc_RuntimeError, "i_camera>=0 should be true");
        return false;
    }

    if( solver_context == NULL ||
        (PyObject*)solver_context == Py_None ||
        Py_TYPE(solver_context) != &SolverContextType)
    {
        PyErr_Format(PyExc_RuntimeError, "solver_context must be of type mrcal.SolverContext");
        return false;
    }
    if(((SolverContext*)solver_context)->ctx == NULL)
    {
        PyErr_Format(PyExc_RuntimeError, "solver_context must contain a non-empty context");
        return false;
    }

    return true;
}
static PyObject* queryIntrinsicOutliernessAt(PyObject* NPY_UNUSED(self),
                                             PyObject* args,
                                             PyObject* kwargs)
{
    PyObject* result = NULL;
    SET_SIGINT();

    QIOA_ARGUMENTS_ALL(ARG_DEFINE) ;
    char* keywords[] = { QIOA_ARGUMENTS_REQUIRED(NAMELIST)
                         QIOA_ARGUMENTS_OPTIONAL(NAMELIST)
                         NULL};

    if(!PyArg_ParseTupleAndKeywords( args, kwargs,
                                     QIOA_ARGUMENTS_REQUIRED(PARSECODE) "|"
                                     QIOA_ARGUMENTS_OPTIONAL(PARSECODE),

                                     keywords,

                                     QIOA_ARGUMENTS_REQUIRED(PARSEARG)
                                     QIOA_ARGUMENTS_OPTIONAL(PARSEARG) NULL))
        goto done;

    if(!queryIntrinsicOutliernessAt_validate_args( QIOA_ARGUMENTS_REQUIRED(ARG_LIST_CALL)
                                                   QIOA_ARGUMENTS_OPTIONAL(ARG_LIST_CALL)
                                                   NULL))
        goto done;

    int N = PyArray_SIZE(v) / 3;
    PyArrayObject* traces = (PyArrayObject*)PyArray_SimpleNew(PyArray_NDIM(v)-1, PyArray_DIMS(v), NPY_DOUBLE);
    void* ctx = solver_context->ctx;
    if(!mrcal_queryIntrinsicOutliernessAt((double*)PyArray_DATA(traces),
                                          solver_context->distortion_model,
                                          solver_context->do_optimize_intrinsic_core,
                                          solver_context->do_optimize_intrinsic_distortions,
                                          solver_context->cahvor_radial_only,
                                          i_camera,
                                          (const union point3_t*)PyArray_DATA(v),
                                          N, Noutliers,
                                          ctx))
    {
        Py_DECREF(traces);
        goto done;
    }

    result = (PyObject*)traces;

 done:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    QIOA_ARGUMENTS_REQUIRED(FREE_PYARRAY) ;
    QIOA_ARGUMENTS_OPTIONAL(FREE_PYARRAY) ;
#pragma GCC diagnostic pop

    RESET_SIGINT();
    return result;
}



#define OPTIMIZE_ARGUMENTS_REQUIRED(_)                                  \
    _(intrinsics,                         PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {-1 COMMA -1       } ) \
    _(extrinsics,                         PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {-1 COMMA  6       } ) \
    _(frames,                             PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {-1 COMMA  6       } ) \
    _(points,                             PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {-1 COMMA  3       } ) \
    _(observations_board,                 PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {-1 COMMA -1 COMMA -1 COMMA -1 } ) \
    _(indices_frame_camera_board,         PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_INT,    {-1 COMMA  2       } ) \
    _(observations_point,                 PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {-1 COMMA  3       } ) \
    _(indices_point_camera_points,        PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_INT,    {-1 COMMA  2       } ) \
    _(distortion_model,                   PyObject*,      NULL,    "S",  ,                                  -1,         {}                   ) \
    _(observed_pixel_uncertainty,         PyObject*,      NULL,    "O",  ,                                  -1,         {}                   ) \
    _(imagersizes,                        PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_INT,    {-1 COMMA 2        } )

#define OPTIMIZE_ARGUMENTS_OPTIONAL(_) \
    _(do_optimize_intrinsic_core,         PyObject*,      Py_True, "O",  ,                                  -1,         {})  \
    _(do_optimize_intrinsic_distortions,  PyObject*,      Py_True, "O",  ,                                  -1,         {})  \
    _(do_optimize_extrinsics,             PyObject*,      Py_True, "O",  ,                                  -1,         {})  \
    _(do_optimize_frames,                 PyObject*,      Py_True, "O",  ,                                  -1,         {})  \
    _(cahvor_radial_only,                 PyObject*,      Py_False,"O",  ,                                  -1,         {})  \
    _(skipped_observations_board,         PyObject*,      NULL,    "O",  ,                                  -1,         {})  \
    _(skipped_observations_point,         PyObject*,      NULL,    "O",  ,                                  -1,         {})  \
    _(calibration_object_spacing,         PyObject*,      NULL,    "O",  ,                                  -1,         {})  \
    _(calibration_object_width_n,         PyObject*,      NULL,    "O",  ,                                  -1,         {})  \
    _(outlier_indices,                    PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_INT,    {-1} ) \
    _(roi,                                PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, NPY_DOUBLE, {-1 COMMA 4} ) \
    _(VERBOSE,                            PyObject*,      NULL,    "O",  ,                                  -1,         {})  \
    _(get_intrinsic_covariances,          PyObject*,      NULL,    "O",  ,                                  -1,         {})  \
    _(skip_outlier_rejection,             PyObject*,      NULL,    "O",  ,                                  -1,         {})  \
    _(skip_regularization,                PyObject*,      NULL,    "O",  ,                                  -1,         {})  \
    _(solver_context,                     SolverContext*, NULL,    "O",  (PyObject*),                       -1,         {})

#define OPTIMIZE_ARGUMENTS_ALL(_) \
    OPTIMIZE_ARGUMENTS_REQUIRED(_) \
    OPTIMIZE_ARGUMENTS_OPTIONAL(_)

static bool optimize_validate_args( // out
                                    enum distortion_model_t* distortion_model_type,

                                    // in
                                    OPTIMIZE_ARGUMENTS_REQUIRED(ARG_LIST_DEFINE)
                                    OPTIMIZE_ARGUMENTS_OPTIONAL(ARG_LIST_DEFINE)

                                    void* dummy __attribute__((unused)))
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    OPTIMIZE_ARGUMENTS_ALL(CHECK_LAYOUT) ;
#pragma GCC diagnostic pop

    int Ncameras = PyArray_DIMS(intrinsics)[0];
    if( Ncameras-1 !=
        PyArray_DIMS(extrinsics)[0] )
    {
        PyErr_Format(PyExc_RuntimeError, "Inconsistent Ncameras: 'extrinsics' says %ld, 'intrinsics' says %ld",
                     PyArray_DIMS(extrinsics)[0] + 1,
                     PyArray_DIMS(intrinsics)[0] );
        return false;
    }
    if( PyArray_DIMS(imagersizes)[0] != Ncameras )
    {
        PyErr_Format(PyExc_RuntimeError, "Inconsistent Ncameras: 'extrinsics' says %ld, 'imagersizes' says %ld",
                     PyArray_DIMS(extrinsics)[0] + 1,
                     PyArray_DIMS(imagersizes)[0]);
        return false;
    }
    if( roi != NULL && (PyObject*)roi != Py_None && PyArray_DIMS(roi)[0] != Ncameras )
    {
        PyErr_Format(PyExc_RuntimeError, "Inconsistent Ncameras: 'extrinsics' says %ld, 'roi' says %ld",
                     PyArray_DIMS(extrinsics)[0] + 1,
                     PyArray_DIMS(roi)[0]);
        return false;
    }

    static_assert( sizeof(struct pose_t)/sizeof(double) == 6, "pose_t is assumed to contain 6 elements");

    long int NobservationsBoard = PyArray_DIMS(observations_board)[0];
    if( PyArray_DIMS(indices_frame_camera_board)[0] != NobservationsBoard )
    {
        PyErr_Format(PyExc_RuntimeError, "Inconsistent NobservationsBoard: 'observations_board' says %ld, 'indices_frame_camera_board' says %ld",
                     NobservationsBoard,
                     PyArray_DIMS(indices_frame_camera_board)[0]);
        return false;
    }

    // calibration_object_spacing and calibration_object_width_n must be > 0 OR
    // we have to not be using a calibration board
    int c_calibration_object_width_n = 0;
    if( NobservationsBoard > 0 )
    {
        if(!PyFloat_Check(calibration_object_spacing))
        {
            PyErr_Format(PyExc_RuntimeError, "We have board observations, so calibration_object_spacing MUST be a valid float > 0");
            return false;
        }

        double c_calibration_object_spacing =
            PyFloat_AS_DOUBLE(calibration_object_spacing);
        if( c_calibration_object_spacing <= 0.0 )
        {
            PyErr_Format(PyExc_RuntimeError, "We have board observations, so calibration_object_spacing MUST be a valid float > 0");
            return false;
        }

        if(!PyInt_Check(calibration_object_width_n))
        {
            PyErr_Format(PyExc_RuntimeError, "We have board observations, so calibration_object_width_n MUST be a valid int > 0");
            return false;
        }

        c_calibration_object_width_n = (int)PyInt_AS_LONG(calibration_object_width_n);
        if( c_calibration_object_width_n <= 0 )
        {
            PyErr_Format(PyExc_RuntimeError, "We have board observations, so calibration_object_width_n MUST be a valid int > 0");
            return false;
        }


        if( c_calibration_object_width_n != PyArray_DIMS(observations_board)[1] ||
            c_calibration_object_width_n != PyArray_DIMS(observations_board)[2] )
        {
            PyErr_Format(PyExc_RuntimeError, "observations_board.shape[1:] MUST be (%d,%d,2). Instead got (%ld,%ld,%ld)",
                         c_calibration_object_width_n, c_calibration_object_width_n,
                         PyArray_DIMS(observations_board)[1],
                         PyArray_DIMS(observations_board)[2],
                         PyArray_DIMS(observations_board)[3]);
            return false;
        }
    }

    long int NobservationsPoint = PyArray_DIMS(observations_point)[0];
    if( PyArray_DIMS(indices_point_camera_points)[0] != NobservationsPoint )
    {
        PyErr_Format(PyExc_RuntimeError, "Inconsistent NobservationsPoint: 'observations_point' says %ld, 'indices_point_camera_points' says %ld",
                     NobservationsPoint,
                     PyArray_DIMS(indices_point_camera_points)[0]);
        return false;
    }

    const char* distortion_model_cstring =
        PyString_AsString(distortion_model);
    if( distortion_model_cstring == NULL)
    {
        PyErr_SetString(PyExc_RuntimeError, "Distortion model was not passed in. Must be a string, one of ("
                        DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                        ")");
        return false;
    }

    *distortion_model_type = mrcal_distortion_model_from_name(distortion_model_cstring);
    if( *distortion_model_type == DISTORTION_INVALID )
    {
        PyErr_Format(PyExc_RuntimeError, "Invalid distortion model was passed in: '%s'. Must be a string, one of ("
                     DISTORTION_LIST( QUOTED_LIST_WITH_COMMA )
                     ")",
                     distortion_model_cstring);
        return false;
    }


    int NdistortionParams = mrcal_getNdistortionParams(*distortion_model_type);
    if( N_INTRINSICS_CORE + NdistortionParams != PyArray_DIMS(intrinsics)[1] )
    {
        PyErr_Format(PyExc_RuntimeError, "intrinsics.shape[1] MUST be %d. Instead got %ld",
                     N_INTRINSICS_CORE + NdistortionParams,
                     PyArray_DIMS(intrinsics)[1] );
        return false;
    }

    if( skipped_observations_board != NULL &&
        skipped_observations_board != Py_None)
    {
        if( !PySequence_Check(skipped_observations_board) )
        {
            PyErr_Format(PyExc_RuntimeError, "skipped_observations_board MUST be None or an iterable of monotonically-increasing integers >= 0");
            return false;
        }

        int Nskipped_observations = (int)PySequence_Size(skipped_observations_board);
        long iskip_last = -1;
        for(int i=0; i<Nskipped_observations; i++)
        {
            PyObject* nextskip = PySequence_GetItem(skipped_observations_board, i);
            if(!PyInt_Check(nextskip))
            {
                PyErr_Format(PyExc_RuntimeError, "skipped_observations_board MUST be None or an iterable of monotonically-increasing integers >= 0");
                return false;
            }
            long iskip = PyInt_AsLong(nextskip);
            if(iskip <= iskip_last)
            {
                PyErr_Format(PyExc_RuntimeError, "skipped_observations_board MUST be None or an iterable of monotonically-increasing integers >= 0");
                return false;
            }
            iskip_last = iskip;
        }
    }

    if( skipped_observations_point != NULL &&
        skipped_observations_point != Py_None)
    {
        if( !PySequence_Check(skipped_observations_point) )
        {
            PyErr_Format(PyExc_RuntimeError, "skipped_observations_point MUST be None or an iterable of monotonically-increasing integers >= 0");
            return false;
        }

        int Nskipped_observations = (int)PySequence_Size(skipped_observations_point);
        long iskip_last = -1;
        for(int i=0; i<Nskipped_observations; i++)
        {
            PyObject* nextskip = PySequence_GetItem(skipped_observations_point, i);
            if(!PyInt_Check(nextskip))
            {
                PyErr_Format(PyExc_RuntimeError, "skipped_observations_point MUST be None or an iterable of monotonically-increasing integers >= 0");
                return false;
            }
            long iskip = PyInt_AsLong(nextskip);
            if(iskip <= iskip_last)
            {
                PyErr_Format(PyExc_RuntimeError, "skipped_observations_point MUST be None or an iterable of monotonically-increasing integers >= 0");
                return false;
            }
            iskip_last = iskip;
        }
    }

    if(!PyFloat_Check(observed_pixel_uncertainty))
    {
        PyErr_Format(PyExc_RuntimeError, "Observed_pixel_uncertainty MUST be a valid float > 0");
        return false;
    }
    double c_observed_pixel_uncertainty =
        PyFloat_AS_DOUBLE(observed_pixel_uncertainty);
    if( c_observed_pixel_uncertainty <= 0.0 )
    {
        PyErr_Format(PyExc_RuntimeError, "Observed_pixel_uncertainty MUST be a valid float > 0");
        return false;
    }

    if( !(solver_context == NULL ||
          (PyObject*)solver_context == Py_None ||
          Py_TYPE(solver_context) == &SolverContextType) )
    {
        PyErr_Format(PyExc_RuntimeError, "solver_context must be None or of type mrcal.SolverContext");
        return false;
    }

    return true;
}
static PyObject* optimize(PyObject* NPY_UNUSED(self),
                          PyObject* args,
                          PyObject* kwargs)
{
    PyObject* result = NULL;
    SET_SIGINT();

    PyArrayObject* x_final                     = NULL;
    PyArrayObject* intrinsic_covariances       = NULL;
    PyArrayObject* outlier_indices_final       = NULL;
    PyArrayObject* outside_ROI_indices_final   = NULL;
    PyObject* pystats = NULL;

    OPTIMIZE_ARGUMENTS_ALL(ARG_DEFINE) ;
    char* keywords[] = { OPTIMIZE_ARGUMENTS_REQUIRED(NAMELIST)
                         OPTIMIZE_ARGUMENTS_OPTIONAL(NAMELIST)
                         NULL};

    if(!PyArg_ParseTupleAndKeywords( args, kwargs,
                                     OPTIMIZE_ARGUMENTS_REQUIRED(PARSECODE) "|"
                                     OPTIMIZE_ARGUMENTS_OPTIONAL(PARSECODE),

                                     keywords,

                                     OPTIMIZE_ARGUMENTS_REQUIRED(PARSEARG)
                                     OPTIMIZE_ARGUMENTS_OPTIONAL(PARSEARG) NULL))
        goto done;


    // Some of my input arguments can be empty (None). The code all assumes that
    // everything is a properly-dimensions numpy array, with "empty" meaning
    // some dimension is 0. Here I make this conversion. The user can pass None,
    // and we still do the right thing.
    //
    // There's a silly implementation detail here: if you have a preprocessor
    // macro M(x), and you pass it M({1,2,3}), the preprocessor see 3 separate
    // args, not 1. That's why I have a __VA_ARGS__ here and why I instantiate a
    // separate dims[] (PyArray_SimpleNew is a macro too)
#define SET_SIZE0_IF_NONE(x, type, ...)                                 \
    ({                                                                  \
        if( x == NULL || Py_None == (PyObject*)x )                      \
        {                                                               \
            if( x != NULL ) Py_DECREF(x);                               \
            npy_intp dims[] = {__VA_ARGS__};                            \
            x = (PyArrayObject*)PyArray_SimpleNew(sizeof(dims)/sizeof(dims[0]), \
                                                  dims, type);          \
        }                                                               \
    })

    SET_SIZE0_IF_NONE(extrinsics,                 NPY_DOUBLE, 0,6);

    SET_SIZE0_IF_NONE(frames,                     NPY_DOUBLE, 0,6);
    SET_SIZE0_IF_NONE(observations_board,         NPY_DOUBLE, 0,179,171,2);
    SET_SIZE0_IF_NONE(indices_frame_camera_board, NPY_INT,    0,2);

    SET_SIZE0_IF_NONE(points,                     NPY_DOUBLE, 0,3);
    SET_SIZE0_IF_NONE(observations_point,         NPY_DOUBLE, 0,3);
    SET_SIZE0_IF_NONE(indices_point_camera_points,NPY_INT,    0,2);
    SET_SIZE0_IF_NONE(imagersizes,                NPY_INT,    0,2);
#undef SET_NULL_IF_NONE




    enum distortion_model_t distortion_model_type;
    if( !optimize_validate_args(&distortion_model_type,
                                OPTIMIZE_ARGUMENTS_REQUIRED(ARG_LIST_CALL)
                                OPTIMIZE_ARGUMENTS_OPTIONAL(ARG_LIST_CALL)
                                NULL))
        goto done;

    {
        int Ncameras           = PyArray_DIMS(intrinsics)[0];
        int Nframes            = PyArray_DIMS(frames)[0];
        int Npoints            = PyArray_DIMS(points)[0];
        int NobservationsBoard = PyArray_DIMS(observations_board)[0];
        int NobservationsPoint = PyArray_DIMS(observations_point)[0];

        double c_calibration_object_spacing  = 0.0;
        int    c_calibration_object_width_n  = 0;

        if( NobservationsBoard )
        {
            if(PyFloat_Check(calibration_object_spacing))
                c_calibration_object_spacing = PyFloat_AS_DOUBLE(calibration_object_spacing);
            if(PyInt_Check(calibration_object_width_n))
                c_calibration_object_width_n = (int)PyInt_AS_LONG(calibration_object_width_n);
        }


        // The checks in optimize_validate_args() make sure these casts are kosher
        double*              c_intrinsics = (double*)         PyArray_DATA(intrinsics);
        struct pose_t*       c_extrinsics = (struct pose_t*)  PyArray_DATA(extrinsics);
        struct pose_t*       c_frames     = (struct pose_t*)  PyArray_DATA(frames);
        union  point3_t*     c_points     = (union  point3_t*)PyArray_DATA(points);



        struct observation_board_t c_observations_board[NobservationsBoard];
        int Nskipped_observations_board =
            ( skipped_observations_board == NULL ||
              skipped_observations_board == Py_None ) ?
            0 :
            (int)PySequence_Size(skipped_observations_board);
        int i_skipped_observation_board = 0;
        int i_observation_board_next_skip = -1;
        if( i_skipped_observation_board < Nskipped_observations_board )
        {
            PyObject* nextskip = PySequence_GetItem(skipped_observations_board, i_skipped_observation_board);
            i_observation_board_next_skip = (int)PyInt_AsLong(nextskip);
        }

        int i_frame_current_skipped = -1;
        int i_frame_last            = -1;
        for(int i_observation=0; i_observation<NobservationsBoard; i_observation++)
        {
            int i_frame  = ((int*)PyArray_DATA(indices_frame_camera_board))[i_observation*2 + 0];
            int i_camera = ((int*)PyArray_DATA(indices_frame_camera_board))[i_observation*2 + 1];

            c_observations_board[i_observation].i_camera         = i_camera;
            c_observations_board[i_observation].i_frame          = i_frame;
            c_observations_board[i_observation].px               = &((union point2_t*)PyArray_DATA(observations_board))[c_calibration_object_width_n*c_calibration_object_width_n*i_observation];

            // I skip this frame if I skip ALL observations of this frame
            if( i_frame_current_skipped >= 0 &&
                i_frame_current_skipped != i_frame )
            {
                // Ooh! We moved past the frame where we skipped all
                // observations. So I need to go back, and mark all of those as
                // skipping that frame
                for(int i_observation_skip_frame = i_observation-1;
                    i_observation_skip_frame >= 0 && c_observations_board[i_observation_skip_frame].i_frame == i_frame_current_skipped;
                    i_observation_skip_frame--)
                {
                    c_observations_board[i_observation_skip_frame].skip_frame = true;
                }
            }
            else
                c_observations_board[i_observation].skip_frame = false;

            if( i_observation == i_observation_board_next_skip )
            {
                if( i_frame_last != i_frame )
                    i_frame_current_skipped = i_frame;

                c_observations_board[i_observation].skip_observation = true;

                i_skipped_observation_board++;
                if( i_skipped_observation_board < Nskipped_observations_board )
                {
                    PyObject* nextskip = PySequence_GetItem(skipped_observations_board, i_skipped_observation_board);
                    i_observation_board_next_skip = (int)PyInt_AsLong(nextskip);
                }
                else
                    i_observation_board_next_skip = -1;
            }
            else
            {
                c_observations_board[i_observation].skip_observation = false;
                i_frame_current_skipped = -1;
            }

            i_frame_last = i_frame;
        }
        // check for frame-skips on the last observation
        if( i_frame_current_skipped >= 0 )
        {
            for(int i_observation_skip_frame = NobservationsBoard - 1;
                i_observation_skip_frame >= 0 && c_observations_board[i_observation_skip_frame].i_frame == i_frame_current_skipped;
                i_observation_skip_frame--)
            {
                c_observations_board[i_observation_skip_frame].skip_frame = true;
            }
        }





        struct observation_point_t c_observations_point[NobservationsPoint];
        int Nskipped_observations_point =
            ( skipped_observations_point == NULL ||
              skipped_observations_point == Py_None ) ?
            0 :
            (int)PySequence_Size(skipped_observations_point);
        int i_skipped_observation_point = 0;
        int i_observation_point_next_skip = -1;
        if( i_skipped_observation_point < Nskipped_observations_point )
        {
            PyObject* nextskip = PySequence_GetItem(skipped_observations_point, i_skipped_observation_point);
            i_observation_point_next_skip = (int)PyInt_AsLong(nextskip);
        }

        int i_point_current_skipped = -1;
        int i_point_last            = -1;
        for(int i_observation=0; i_observation<NobservationsPoint; i_observation++)
        {
            int i_point  = ((int*)PyArray_DATA(indices_point_camera_points))[i_observation*2 + 0];
            int i_camera = ((int*)PyArray_DATA(indices_point_camera_points))[i_observation*2 + 1];

            c_observations_point[i_observation].i_camera         = i_camera;
            c_observations_point[i_observation].i_point          = i_point;
            c_observations_point[i_observation].px               = *(union point2_t*)(&((double*)PyArray_DATA(observations_point))[i_observation*3]);
            c_observations_point[i_observation].dist             = ((double*)PyArray_DATA(observations_point))[i_observation*3 + 2];

            // I skip this point if I skip ALL observations of this point
            if( i_point_current_skipped >= 0 &&
                i_point_current_skipped != i_point )
            {
                // Ooh! We moved past the point where we skipped all
                // observations. So I need to go back, and mark all of those as
                // skipping that point
                for(int i_observation_skip_point = i_observation-1;
                    i_observation_skip_point >= 0 && c_observations_point[i_observation_skip_point].i_point == i_point_current_skipped;
                    i_observation_skip_point--)
                {
                    c_observations_point[i_observation_skip_point].skip_point = true;
                }
            }
            else
                c_observations_point[i_observation].skip_point = false;

            if( i_observation == i_observation_point_next_skip )
            {
                if( i_point_last != i_point )
                    i_point_current_skipped = i_point;

                c_observations_point[i_observation].skip_observation = true;

                i_skipped_observation_point++;
                if( i_skipped_observation_point < Nskipped_observations_point )
                {
                    PyObject* nextskip = PySequence_GetItem(skipped_observations_point, i_skipped_observation_point);
                    i_observation_point_next_skip = (int)PyInt_AsLong(nextskip);
                }
                else
                    i_observation_point_next_skip = -1;
            }
            else
            {
                c_observations_point[i_observation].skip_observation = false;
                i_point_current_skipped = -1;
            }

            i_point_last = i_point;
        }
        // check for point-skips on the last observation
        if( i_point_current_skipped >= 0 )
        {
            for(int i_observation_skip_point = NobservationsPoint - 1;
                i_observation_skip_point >= 0 && c_observations_point[i_observation_skip_point].i_point == i_point_current_skipped;
                i_observation_skip_point--)
            {
                c_observations_point[i_observation_skip_point].skip_point = true;
            }
        }





        mrcal_problem_details_t problem_details =
            { .do_optimize_intrinsic_core        = PyObject_IsTrue(do_optimize_intrinsic_core),
              .do_optimize_intrinsic_distortions = PyObject_IsTrue(do_optimize_intrinsic_distortions),
              .do_optimize_extrinsics            = PyObject_IsTrue(do_optimize_extrinsics),
              .do_optimize_frames                = PyObject_IsTrue(do_optimize_frames),
              .cahvor_radial_only                = PyObject_IsTrue(cahvor_radial_only),
              .do_skip_regularization            = skip_regularization && PyObject_IsTrue(skip_regularization)
            };

        int Nmeasurements = mrcal_getNmeasurements(Ncameras, NobservationsBoard,
                                                   c_observations_point, NobservationsPoint,
                                                   c_calibration_object_width_n,
                                                   problem_details,
                                                   distortion_model_type);

        x_final = (PyArrayObject*)PyArray_SimpleNew(1, ((npy_intp[]){Nmeasurements}), NPY_DOUBLE);
        double* c_x_final = PyArray_DATA(x_final);

        int Nintrinsics_all = mrcal_getNintrinsicParams(distortion_model_type);
        double* c_intrinsic_covariances = NULL;
        if(Nintrinsics_all != 0 &&
           get_intrinsic_covariances && PyObject_IsTrue(get_intrinsic_covariances))
        {
            intrinsic_covariances =
                (PyArrayObject*)PyArray_SimpleNew(3,
                                                  ((npy_intp[]){Ncameras,Nintrinsics_all,Nintrinsics_all}), NPY_DOUBLE);
            c_intrinsic_covariances = PyArray_DATA(intrinsic_covariances);
        }

        const int Npoints_fromBoards =
            NobservationsBoard *
            c_calibration_object_width_n*c_calibration_object_width_n;
        outlier_indices_final     = (PyArrayObject*)PyArray_SimpleNew(1, ((npy_intp[]){Npoints_fromBoards}), NPY_INT);
        outside_ROI_indices_final = (PyArrayObject*)PyArray_SimpleNew(1, ((npy_intp[]){Npoints_fromBoards}), NPY_INT);

        // output
        int* c_outlier_indices_final     = PyArray_DATA(outlier_indices_final);
        int* c_outside_ROI_indices_final = PyArray_DATA(outside_ROI_indices_final);
        // input
        int* c_outlier_indices;
        int Noutlier_indices;
        if(outlier_indices == NULL || (PyObject*)outlier_indices == Py_None)
        {
            c_outlier_indices = NULL;
            Noutlier_indices  = 0;
        }
        else
        {
            c_outlier_indices = PyArray_DATA(outlier_indices);
            Noutlier_indices = PyArray_DIMS(outlier_indices)[0];
        }

        double* c_roi;
        if(roi == NULL || (PyObject*)roi == Py_None)
            c_roi = NULL;
        else
            c_roi = PyArray_DATA(roi);

        int* c_imagersizes = PyArray_DATA(imagersizes);
        double c_observed_pixel_uncertainty = PyFloat_AS_DOUBLE(observed_pixel_uncertainty);

        void** solver_context_optimizer = NULL;
        if(solver_context != NULL && (PyObject*)solver_context != Py_None)
        {
            solver_context_optimizer = &solver_context->ctx;
            solver_context->distortion_model = distortion_model_type;
            solver_context->do_optimize_intrinsic_core =
                problem_details.do_optimize_intrinsic_core;
            solver_context->do_optimize_intrinsic_distortions =
                problem_details.do_optimize_intrinsic_distortions;
            solver_context->cahvor_radial_only =
                problem_details.cahvor_radial_only;
        }


        struct mrcal_stats_t stats =
        mrcal_optimize( c_x_final,
                        c_intrinsic_covariances,
                        c_outlier_indices_final,
                        c_outside_ROI_indices_final,
                        solver_context_optimizer,
                        c_intrinsics,
                        c_extrinsics,
                        c_frames,
                        c_points,
                        Ncameras, Nframes, Npoints,

                        c_observations_board,
                        NobservationsBoard,
                        c_observations_point,
                        NobservationsPoint,

                        false,
                        Noutlier_indices,
                        c_outlier_indices,
                        c_roi,
                        VERBOSE &&                PyObject_IsTrue(VERBOSE),
                        skip_outlier_rejection && PyObject_IsTrue(skip_outlier_rejection),
                        distortion_model_type,
                        c_observed_pixel_uncertainty,
                        c_imagersizes,
                        problem_details,

                        c_calibration_object_spacing,
                        c_calibration_object_width_n);
        pystats = PyDict_New();
        if(pystats == NULL)
        {
            PyErr_SetString(PyExc_RuntimeError, "PyDict_New() failed!");
            goto done;
        }
    #define MRCAL_STATS_ITEM_POPULATE_DICT(type, name, pyconverter)     \
        {                                                               \
            PyObject* obj = pyconverter( (type)stats.name);             \
            if( obj == NULL)                                            \
            {                                                           \
                PyErr_SetString(PyExc_RuntimeError, "Couldn't make PyObject for '" #name "'"); \
                goto done;                                              \
            }                                                           \
                                                                        \
            if( 0 != PyDict_SetItemString(pystats, #name, obj) )        \
            {                                                           \
                PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict '" #name "'"); \
                Py_DECREF(obj);                                         \
                goto done;                                              \
            }                                                           \
        }
        MRCAL_STATS_ITEM(MRCAL_STATS_ITEM_POPULATE_DICT);

        if( 0 != PyDict_SetItemString(pystats, "x",
                                      (PyObject*)x_final) )
        {
            PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict 'x'");
            goto done;
        }
        if( intrinsic_covariances &&
            0 != PyDict_SetItemString(pystats, "intrinsic_covariances",
                                      (PyObject*)intrinsic_covariances) )
        {
            PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict 'intrinsic_covariances'");
            goto done;
        }
        // The outlier_indices_final numpy array has Nfeatures elements,
        // but I want to return only the first Noutliers elements
        if( NULL == PyArray_Resize(outlier_indices_final,
                                   &(PyArray_Dims){ .ptr = ((npy_intp[]){stats.Noutliers}),
                                                    .len = 1 },
                                   true,
                                   NPY_ANYORDER))
        {
            PyErr_Format(PyExc_RuntimeError, "Couldn't resize outlier_indices_final to %d elements",
                         stats.Noutliers);
            goto done;
        }
        if( 0 != PyDict_SetItemString(pystats, "outlier_indices",
                                      (PyObject*)outlier_indices_final) )
        {
            PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict 'outlier_indices'");
            goto done;
        }
        // The outside_ROI_indices_final numpy array has Nfeatures elements,
        // but I want to return only the first NoutsideROI elements
        if( NULL == PyArray_Resize(outside_ROI_indices_final,
                                   &(PyArray_Dims){ .ptr = ((npy_intp[]){stats.NoutsideROI}),
                                                    .len = 1 },
                                   true,
                                   NPY_ANYORDER))
        {
            PyErr_Format(PyExc_RuntimeError, "Couldn't resize outside_ROI_indices_final to %d elements",
                         stats.NoutsideROI);
            goto done;
        }
        if( 0 != PyDict_SetItemString(pystats, "outside_ROI_indices",
                                      (PyObject*)outside_ROI_indices_final) )
        {
            PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict 'outside_ROI_indices'");
            goto done;
        }

        result = pystats;
        Py_INCREF(result);
    }

 done:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    OPTIMIZE_ARGUMENTS_REQUIRED(FREE_PYARRAY) ;
    OPTIMIZE_ARGUMENTS_OPTIONAL(FREE_PYARRAY) ;
#pragma GCC diagnostic pop

    if(x_final)               Py_DECREF(x_final);
    if(intrinsic_covariances) Py_DECREF(intrinsic_covariances);
    if(outlier_indices_final) Py_DECREF(outlier_indices_final);
    if(pystats)               Py_DECREF(pystats);

    RESET_SIGINT();
    return result;
}

PyMODINIT_FUNC init_mrcal(void)
{
    static const char optimize_docstring[] =
#include "optimize.docstring.h"
        ;
    static const char getNdistortionParams_docstring[] =
#include "getNdistortionParams.docstring.h"
        ;
    static const char getSupportedDistortionModels_docstring[] =
#include "getSupportedDistortionModels.docstring.h"
        ;
    static const char getNextDistortionModel_docstring[] =
#include "getNextDistortionModel.docstring.h"
        ;
    static const char project_docstring[] =
#include "project.docstring.h"
        ;
    static const char queryIntrinsicOutliernessAt_docstring[] =
#include "queryIntrinsicOutliernessAt.docstring.h"
        ;

#define PYMETHODDEF_ENTRY(x, args) {#x, (PyCFunction)x, args, x ## _docstring}
    static PyMethodDef methods[] =
        { PYMETHODDEF_ENTRY(optimize,                     METH_VARARGS | METH_KEYWORDS),
          PYMETHODDEF_ENTRY(getNdistortionParams,         METH_VARARGS),
          PYMETHODDEF_ENTRY(getSupportedDistortionModels, METH_NOARGS),
          PYMETHODDEF_ENTRY(getNextDistortionModel,       METH_VARARGS),
          PYMETHODDEF_ENTRY(project,                      METH_VARARGS | METH_KEYWORDS),
          PYMETHODDEF_ENTRY(queryIntrinsicOutliernessAt,  METH_VARARGS | METH_KEYWORDS),
          {}
        };

    if (PyType_Ready(&SolverContextType) < 0)
        return;

    PyImport_AddModule("_mrcal");
    PyObject* module = Py_InitModule3("_mrcal", methods,
                                      "Calibration and SFM routines");

    Py_INCREF(&SolverContextType);
    PyModule_AddObject(module, "SolverContext", (PyObject *)&SolverContextType);

    import_array();
}
