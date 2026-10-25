/*
 *  Copyright (c) 2026 blueloveTH
 *  Distributed Under The MIT License
 *  https://github.com/pocketpy/pocketpy
 */
 
#pragma once


#define PK_IS_PUBLIC_INCLUDE

// clang-format off

#define PK_VERSION				"2.2.0"
#define PK_VERSION_MAJOR            2
#define PK_VERSION_MINOR            2
#define PK_VERSION_PATCH            0

/*************** feature settings ***************/
#ifndef PK_ENABLE_OS                // can be overridden by cmake
#define PK_ENABLE_OS                1
#endif

#ifndef PK_ENABLE_THREADS           // must be enabled from cmake
#define PK_ENABLE_THREADS           0
#endif

#ifndef PK_ENABLE_DLL               // must be enabled from cmake
#define PK_ENABLE_DLL               0
#endif

#ifndef PK_ENABLE_DETERMINISM       // must be enabled from cmake
#define PK_ENABLE_DETERMINISM       0
#endif

#ifndef PK_ENABLE_WATCHDOG          // can be overridden by cmake
#define PK_ENABLE_WATCHDOG          0                
#endif

#ifndef PK_ENABLE_CUSTOM_SNAME      // can be overridden by cmake
#define PK_ENABLE_CUSTOM_SNAME      0                
#endif

#ifndef PK_ENABLE_MIMALLOC          // can be overridden by cmake
#define PK_ENABLE_MIMALLOC          0                
#endif

// GC min threshold
#ifndef PK_GC_MIN_THRESHOLD         // can be overridden by cmake
    #define PK_GC_MIN_THRESHOLD     20000
#endif

// This is the maximum size of the value stack in py_TValue units
// The actual size in bytes equals `sizeof(py_TValue) * PK_VM_STACK_SIZE`
#ifndef PK_VM_STACK_SIZE            // can be overridden by cmake
    #define PK_VM_STACK_SIZE        16384
#endif

// This is the maximum number of local variables in a function
// (not recommended to change this)
#ifndef PK_MAX_CO_VARNAMES          // can be overridden by cmake
#define PK_MAX_CO_VARNAMES          64
#endif

/*************** internal settings ***************/
// This is the maximum character length of a module path
#define PK_MAX_MODULE_PATH_LEN      63

// Hash table load factor (smaller ones mean less collision but more memory)
// For class instance
#define PK_INST_ATTR_LOAD_FACTOR    0.67f
// For class itself
#define PK_TYPE_ATTR_LOAD_FACTOR    0.5f

#ifdef _WIN32
    #define PK_PLATFORM_SEP '\\'
#else
    #define PK_PLATFORM_SEP '/'
#endif

#ifdef __cplusplus
    #ifndef restrict
        #define restrict
    #endif
#endif

#if PK_ENABLE_THREADS
    #define PK_THREAD_LOCAL _Thread_local
#else
    #define PK_THREAD_LOCAL
#endif

// Memory allocation functions
#ifndef PK_MALLOC
    #if PK_ENABLE_MIMALLOC
        #include "mimalloc.h"
        #define PK_MALLOC(size)                 mi_malloc(size)
        #define PK_REALLOC(ptr, size)           mi_realloc(ptr, size)
        #define PK_FREE(ptr)                    mi_free(ptr)
    #else
        #ifndef __cplusplus
            #include <stdlib.h>
            #define PK_MALLOC(size)             malloc(size)
            #define PK_REALLOC(ptr, size)       realloc(ptr, size)
            #define PK_FREE(ptr)                free(ptr)
        #else
            #include <cstdlib>
            #define PK_MALLOC(size)             std::malloc(size)
            #define PK_REALLOC(ptr, size)       std::realloc(ptr, size)
            #define PK_FREE(ptr)                std::free(ptr)
        #endif
    #endif
#endif

// ---- MANTLE PATCH (see VERSION): forward declarations for the host-supplied allocator
// hooks. PocketPyLib.Build.cs defines PK_MALLOC(size)/PK_REALLOC(ptr,size)/PK_FREE(ptr) as
// function-like macros expanding to these three calls (implemented in
// PocketPyLib/Private/MantlePocketPyAlloc.c) -- pocketpy's own documented extension point for a
// custom allocator. C11 requires a visible prototype before first use; PocketPy's public API
// has no *runtime* allocator-callback parameter (unlike Luau's lua_newstate(allocator, ud)),
// only this compile-time macro seam, so a prototype has to live somewhere pocketpy.c's
// translation unit sees before use. This is the first of two patches in this vendoring (the
// second, a deterministic per-opcode instruction-budget interrupt, lives in pocketpy.c's own
// VM__run_top_frame -- see its own MANTLE PATCH marker and PocketPyLib/VERSION). Declared via
// MantlePocketPyHooks.h (not repeated here) so a translation unit that includes both this file
// and MantlePocketPyHooks.h -- as MantlePocketPy.cpp does -- sees one declaration with
// consistent linkage (POCKETPY_HOOKS_API is dllimport/dllexport for modular Windows targets; a
// second, plain declaration here previously conflicted with that under MSVC, error C2375).
#include "MantlePocketPyHooks.h"
// ---- END MANTLE PATCH ----



// clang-format off

#if defined(_WIN32) || defined(_WIN64)
    #ifdef PY_DYNAMIC_MODULE
        #define PK_API __declspec(dllimport)
    #else
        #define PK_API __declspec(dllexport)
    #endif
    #define PK_EXPORT __declspec(dllexport)
    #define PY_SYS_PLATFORM     0
    #define PY_SYS_PLATFORM_STRING "win32"
#elif __EMSCRIPTEN__
    #define PK_API
    #define PK_EXPORT
    #define PY_SYS_PLATFORM     1
    #define PY_SYS_PLATFORM_STRING "emscripten"
#elif __APPLE__
    #include <TargetConditionals.h>
    #if TARGET_IPHONE_SIMULATOR
        // iOS, tvOS, or watchOS Simulator
        #define PY_SYS_PLATFORM     2
        #define PY_SYS_PLATFORM_STRING "ios"
    #elif TARGET_OS_IPHONE
        // iOS, tvOS, or watchOS device
        #define PY_SYS_PLATFORM     2
        #define PY_SYS_PLATFORM_STRING "ios"
    #elif TARGET_OS_MAC
        #define PY_SYS_PLATFORM     3
        #define PY_SYS_PLATFORM_STRING "darwin"
    #else
    #   error "Unknown Apple platform"
    #endif
    #define PK_API __attribute__((visibility("default")))
    #define PK_EXPORT __attribute__((visibility("default")))
#elif __ANDROID__
    #define PK_API __attribute__((visibility("default")))
    #define PK_EXPORT __attribute__((visibility("default")))
    #define PY_SYS_PLATFORM     4
    #define PY_SYS_PLATFORM_STRING "android"
#elif __linux__
    #define PK_API __attribute__((visibility("default")))
    #define PK_EXPORT __attribute__((visibility("default")))
    #define PY_SYS_PLATFORM     5
    #define PY_SYS_PLATFORM_STRING "linux"
#else
    #define PK_API
    #define PY_SYS_PLATFORM     6
    #define PY_SYS_PLATFORM_STRING "unknown"
#endif

#if PY_SYS_PLATFORM == 0 || PY_SYS_PLATFORM == 3 || PY_SYS_PLATFORM == 5
    #define PK_IS_DESKTOP_PLATFORM 1
#else
    #define PK_IS_DESKTOP_PLATFORM 0
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define PK_DEPRECATED __attribute__((deprecated))
#else
    #define PK_DEPRECATED
#endif

#ifdef NDEBUG
    #if defined(__GNUC__)
        #define PK_INLINE __attribute__((always_inline)) inline
    #elif defined(_MSC_VER) && !defined(__clang__)
        #define PK_INLINE __forceinline
    #else
        #define PK_INLINE inline
    #endif
#else
    #define PK_INLINE
#endif



#include <stdint.h>

typedef union c11_vec2i {
    struct { int x, y; };
    int data[2];
    int64_t _i64;
} c11_vec2i;

typedef union c11_vec3i {
    struct { int x, y, z; };
    int data[3];
} c11_vec3i;

typedef union c11_vec4i {
    struct { int x, y, z, w; };
    int data[4];
} c11_vec4i;

typedef union c11_vec2 {
    struct { float x, y; };
    float data[2];
} c11_vec2;

typedef union c11_vec3 {
    struct { float x, y, z; };
    float data[3];
} c11_vec3;

typedef union c11_mat3x3 {
    struct {
        float _11, _12, _13;
        float _21, _22, _23;
        float _31, _32, _33;
    };

    float m[3][3];
    float data[9];
} c11_mat3x3;

typedef union c11_color32 {
    struct {
        unsigned char r;
        unsigned char g;
        unsigned char b;
        unsigned char a;
    };
    unsigned char data[4];
    uint32_t u32;
} c11_color32;



#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/************* Public Types *************/
/// A helper struct for `py_Name`.
typedef struct py_OpaqueName py_OpaqueName;
/// A pointer that represents a python identifier. For fast name resolution.
typedef py_OpaqueName* py_Name;
/// A opaque type that represents a python object. You cannot access its members directly.
typedef struct py_TValue py_TValue;
/// An integer that represents a python type. `0` is invalid.
typedef int16_t py_Type;
/// A 64-bit integer type. Corresponds to `int` in python.
typedef int64_t py_i64;
/// A 64-bit floating-point type. Corresponds to `float` in python.
typedef double py_f64;
/// A generic destructor function.
typedef void (*py_Dtor)(void*);

/// A string view type. It is helpful for passing strings which are not null-terminated.
typedef struct c11_sv {
    const char* data;
    int size;
} c11_sv;

#define PY_RAISE
#define PY_RETURN
#define PY_MAYBENULL

/// A generic reference to a python object.
typedef py_TValue* py_Ref;
/// A reference which has the same lifespan as the python object.
typedef py_TValue* py_ObjectRef;
/// A global reference which has the same lifespan as the VM.
typedef py_TValue* py_GlobalRef;
/// A specific location in the value stack of the VM.
typedef py_TValue* py_StackRef;
/// An item reference to a container object. It invalidates when the container is modified.
typedef py_TValue* py_ItemRef;
/// An output reference for returning a value. Only use this for function arguments.
typedef py_TValue* py_OutRef;

typedef struct py_Frame py_Frame;

// An enum for tracing events.
enum py_TraceEvent {
    TRACE_EVENT_LINE,
    TRACE_EVENT_PUSH,
    TRACE_EVENT_POP,
};

typedef void (*py_TraceFunc)(py_Frame* frame, enum py_TraceEvent);

/// A struct contains the callbacks of the VM.
typedef struct py_Callbacks {
    // MANTLE PATCH (#123): optional per-VM capability check, before any import cache lookup.
    // Rejected imports appear absent (0); NULL preserves upstream behavior.
    bool (*import_allowed)(const char* path);
    /// Used by `__import__` to load a source or compiled module.
    char* (*importfile)(const char* path, int* data_size);
    /// Called before `importfile` to lazy-import a C module.
    PY_MAYBENULL py_GlobalRef (*lazyimport)(const char*);
    /// Used by `print` to output a string.
    void (*print)(const char*);
    /// Flush the output buffer of `print`.
    void (*flush)();
    /// Used by `input` to get a character.
    int (*getchr)();
    /// Used by `gc.collect()` to mark extra objects for garbage collection.
    PY_MAYBENULL void (*gc_mark)(void (*f)(py_Ref val, void* ctx), void* ctx);
    /// Used by `PRINT_EXPR` bytecode.
    PY_MAYBENULL bool (*displayhook)(py_Ref val) PY_RAISE;
} py_Callbacks;

/// A struct contains the application-level callbacks.
typedef struct py_AppCallbacks {
    void (*on_vm_ctor)(int index);
    void (*on_vm_dtor)(int index);
} py_AppCallbacks;

/// Native function signature.
/// @param argc number of arguments.
/// @param argv array of arguments. Use `py_arg(i)` macro to get the i-th argument.
/// @return `true` if the function is successful or `false` if an exception is raised.
typedef bool (*py_CFunction)(int argc, py_StackRef argv) PY_RAISE PY_RETURN;

/// Python compiler modes.
/// + `EXEC_MODE`: for statements.
/// + `EVAL_MODE`: for expressions.
/// + `SINGLE_MODE`: for REPL or jupyter notebook execution.
/// + `RELOAD_MODE`: for reloading a module without allocating new types if possible.
enum py_CompileMode { EXEC_MODE, EVAL_MODE, SINGLE_MODE, RELOAD_MODE };

/************* Global Setup *************/

/// Initialize pocketpy and the default VM.
PK_API void py_initialize();
/// Finalize pocketpy and free all VMs. This opearation is irreversible.
/// After this call, you cannot use any function from this header anymore.
PK_API void py_finalize();
/// Get the current VM index.
PK_API int py_currentvm();
/// Switch to a VM.
/// @param index index of the VM ranging from 0 to 16 (exclusive). `0` is the default VM.
PK_API void py_switchvm(int index);
/// Reset the current VM.
PK_API void py_resetvm();
/// Reset All VMs.
PK_API void py_resetallvm();
/// Get the current VM context. This is used for user-defined data.
PK_API void* py_getvmctx();
/// Set the current VM context. This is used for user-defined data.
PK_API void py_setvmctx(void* ctx);
/// Setup the callbacks for the current VM.
PK_API py_Callbacks* py_callbacks();
/// Setup the application callbacks
PK_API py_AppCallbacks* py_appcallbacks();

/// Set `sys.argv`. Used for storing command-line arguments.
PK_API void py_sys_setargv(int argc, char** argv);
/// Set the trace function for the current VM.
PK_API void py_sys_settrace(py_TraceFunc func, bool reset);
/// Invoke the garbage collector.
PK_API int py_gc_collect();

/// Wrapper for `PK_MALLOC(size)`.
PK_API void* py_malloc(size_t size);
/// Wrapper for `PK_REALLOC(ptr, size)`.
PK_API void* py_realloc(void* ptr, size_t size);
/// Wrapper for `PK_FREE(ptr)`.
PK_API void py_free(void* ptr);

/// A shorthand for `True`.
PK_API py_GlobalRef py_True();
/// A shorthand for `False`.
PK_API py_GlobalRef py_False();
/// A shorthand for `None`.
PK_API py_GlobalRef py_None();
/// A shorthand for `nil`. `nil` is not a valid python object.
PK_API py_GlobalRef py_NIL();

/************* Frame Ops *************/

/// Get the current source location of the frame.
PK_API const char* py_Frame_sourceloc(py_Frame* frame, int* lineno);
/// Python equivalent to `globals()` with respect to the given frame.
PK_API void py_Frame_newglobals(py_Frame* frame, py_OutRef out);
/// Python equivalent to `locals()` with respect to the given frame.
PK_API void py_Frame_newlocals(py_Frame* frame, py_OutRef out);
/// Get the function object of the frame.
/// Returns `NULL` if not available.
PK_API py_StackRef py_Frame_function(py_Frame* frame);

/************* Code Execution *************/

/// Compile a source string into a code object.
/// Use python's `exec()` or `eval()` to execute it.
PK_API bool py_compile(const char* source,
                       const char* filename,
                       enum py_CompileMode mode,
                       bool is_dynamic) PY_RAISE PY_RETURN;
/// Compile a `.py` file into a `.pyc` file.
PK_API bool py_compilefile(const char* src_path,
                           const char* dst_path) PY_RAISE;
/// Run a compiled code object.
PK_API bool py_execo(const void* data, int size, const char* filename, py_Ref module) PY_RAISE PY_RETURN;
/// Run a source string.
/// @param source source string.
/// @param filename filename (for error messages).
/// @param mode compile mode. Use `EXEC_MODE` for statements `EVAL_MODE` for expressions.
/// @param module target module. Use NULL for the main module.
/// @return `true` if the execution is successful or `false` if an exception is raised.
PK_API bool py_exec(const char* source,
                    const char* filename,
                    enum py_CompileMode mode,
                    py_Ref module) PY_RAISE PY_RETURN;
/// Evaluate a source string. Equivalent to `py_exec(source, "<string>", EVAL_MODE, module)`.
PK_API bool py_eval(const char* source, py_Ref module) PY_RAISE PY_RETURN;
/// Run a source string with smart interpretation.
/// Example:
/// `py_newstr(py_r0(), "abc");`
/// `py_newint(py_r1(), 123);`
/// `py_smartexec("print(_0, _1)", NULL, py_r0(), py_r1());`
/// `// "abc 123" will be printed`.
PK_API bool py_smartexec(const char* source, py_Ref module, ...) PY_RAISE PY_RETURN;
/// Evaluate a source string with smart interpretation.
/// Example:
/// `py_newstr(py_r0(), "abc");`
/// `py_smarteval("len(_)", NULL, py_r0());`
/// `int res = py_toint(py_retval());`
/// `// res will be 3`.
PK_API bool py_smarteval(const char* source, py_Ref module, ...) PY_RAISE PY_RETURN;

/************* Value Creation *************/

/// Create an `int` object.
PK_API void py_newint(py_OutRef, py_i64);
/// Create a trivial value object.
PK_API void py_newtrivial(py_OutRef out, py_Type type, void* data, int size);
/// Create a `float` object.
PK_API void py_newfloat(py_OutRef, py_f64);
/// Create a `bool` object.
PK_API void py_newbool(py_OutRef, bool);
/// Create a `str` object from a null-terminated string (utf-8).
PK_API void py_newstr(py_OutRef, const char*);
/// Create a `str` object with `n` UNINITIALIZED bytes plus `'\0'`.
PK_API char* py_newstrn(py_OutRef, int);
/// Create a `str` object from a `c11_sv`.
PK_API void py_newstrv(py_OutRef, c11_sv);
/* #153: admitted managed-string constructors for explicitly converted callers.
 * Failure records the fixed allocation terminal and leaves out/out_data
 * unchanged. py_trynewstrn publishes uninitialized bytes only on success. */
PK_API bool py_trynewstrn(py_OutRef out, int size, char** out_data);
PK_API bool py_trynewstrv(py_OutRef out, c11_sv sv);
/// Create a formatted `str` object.
PK_API void py_newfstr(py_OutRef, const char*, ...);
/// Create a `bytes` object with `n` UNINITIALIZED bytes.
PK_API unsigned char* py_newbytes(py_OutRef, int n);
/* #153: admitted bytes constructor for explicitly converted core callers.
 * Failure records the fixed allocation terminal and leaves out/out_data
 * unchanged. The uninitialized payload is published only on success. */
PK_API bool py_trynewbytesn(py_OutRef out, int size, unsigned char** out_data);
/// Create a `None` object.
PK_API void py_newnone(py_OutRef);
/// Create a `NotImplemented` object.
PK_API void py_newnotimplemented(py_OutRef);
/// Create a `...` object.
PK_API void py_newellipsis(py_OutRef);
/// Create a `nil` object. `nil` is an invalid representation of an object.
/// Don't use it unless you know what you are doing.
PK_API void py_newnil(py_OutRef);
/// Create a `nativefunc` object.
PK_API void py_newnativefunc(py_OutRef, py_CFunction);
/// Create a `function` object.
PK_API py_Name py_newfunction(py_OutRef out,
                              const char* sig,
                              py_CFunction f,
                              const char* docstring,
                              int slots);
/// Create a `boundmethod` object.
PK_API void py_newboundmethod(py_OutRef out, py_Ref self, py_Ref func);
/// Create a `boundmethod` object without modifying `out` on allocation failure.
PK_API bool py_trynewboundmethod(py_OutRef out, py_Ref self, py_Ref func);
/// Create a new object.
/// @param out output reference.
/// @param type type of the object.
/// @param slots number of slots. Use `-1` to create a `__dict__`.
/// @param udsize size of your userdata.
/// @return pointer to the userdata.
PK_API void* py_newobject(py_OutRef out, py_Type type, int slots, int udsize);
/* MANTLE #153: admitted object construction. `type` must be a registered
 * concrete type (tp_nil < type < current types.length); the requested slot and
 * userdata layout must match that type's runtime use and destructor contract.
 * Both outputs remain unchanged on failure, including slots==-1 NameDict
 * backing denial. */
PK_API bool py_trynewobject(py_OutRef out, py_Type type, int slots, int udsize,
                            void** out_userdata);

/************* Name Conversion *************/

/// Convert a null-terminated string to a name.
PK_API py_Name py_name(const char*);
/// Convert a name to a null-terminated string.
PK_API const char* py_name2str(py_Name);
/// Convert a name to a python `str` object with cache.
PK_API py_GlobalRef py_name2ref(py_Name);
/// Convert a `c11_sv` to a name.
PK_API py_Name py_namev(c11_sv);
/// Convert a name to a `c11_sv`.
PK_API c11_sv py_name2sv(py_Name);

/************* Bindings *************/

/// Bind a function to the object via "decl-based" style.
/// @param obj the target object.
/// @param sig signature of the function. e.g. `add(x, y)`.
/// @param f function to bind.
PK_API void py_bind(py_Ref obj, const char* sig, py_CFunction f);
/// Bind a method to type via "argc-based" style.
/// @param type the target type.
/// @param name name of the method.
/// @param f function to bind.
PK_API void py_bindmethod(py_Type type, const char* name, py_CFunction f);
/// Bind a static method to type via "argc-based" style.
/// @param type the target type.
/// @param name name of the method.
/// @param f function to bind.
PK_API void py_bindstaticmethod(py_Type type, const char* name, py_CFunction f);
/// Bind a function to the object via "argc-based" style.
/// @param obj the target object.
/// @param name name of the function.
/// @param f function to bind.
PK_API void py_bindfunc(py_Ref obj, const char* name, py_CFunction f);
/// Bind a property to type.
/// @param type the target type.
/// @param name name of the property.
/// @param getter getter function.
/// @param setter setter function. Use `NULL` if not needed.
PK_API void
    py_bindproperty(py_Type type, const char* name, py_CFunction getter, py_CFunction setter);
/// Bind a magic method to type.
PK_API void py_bindmagic(py_Type type, py_Name name, py_CFunction f);

/************* Value Cast *************/

/// Convert an `int` object in python to `int64_t`.
PK_API py_i64 py_toint(py_Ref);
/// Get the address of the trivial value object (16 bytes).
PK_API void* py_totrivial(py_Ref);
/// Convert a `float` object in python to `double`.
PK_API py_f64 py_tofloat(py_Ref);
/// Cast a `int` or `float` object in python to `double`.
/// If successful, return true and set the value to `out`.
/// Otherwise, return false and raise `TypeError`.
PK_API bool py_castfloat(py_Ref, py_f64* out) PY_RAISE;
/// 32-bit version of `py_castfloat`.
PK_API bool py_castfloat32(py_Ref, float* out) PY_RAISE;
/// Cast a `int` object in python to `int64_t`.
PK_API bool py_castint(py_Ref, py_i64* out) PY_RAISE;
/// Convert a `bool` object in python to `bool`.
PK_API bool py_tobool(py_Ref);
/// Convert a `type` object in python to `py_Type`.
PK_API py_Type py_totype(py_Ref);
/// Convert a user-defined object to its userdata.
PK_API void* py_touserdata(py_Ref);
/// Convert a `str` object in python to null-terminated string.
PK_API const char* py_tostr(py_Ref);
/// Convert a `str` object in python to char array.
PK_API const char* py_tostrn(py_Ref, int* size);
/// Convert a `str` object in python to `c11_sv`.
PK_API c11_sv py_tosv(py_Ref);
/// Convert a `bytes` object in python to char array.
PK_API unsigned char* py_tobytes(py_Ref, int* size);
/// Resize a `bytes` object. It can only be resized down.
PK_API void py_bytes_resize(py_Ref, int size);

/************* Type System *************/

/// Create a new type.
/// @param name name of the type.
/// @param base base type.
/// @param module module where the type is defined. Use `NULL` for built-in types.
/// @param dtor destructor function. Use `NULL` if not needed.
PK_API py_Type py_newtype(const char* name, py_Type base, const py_GlobalRef module, py_Dtor dtor);
/// Check if the object is exactly the given type.
PK_API bool py_istype(py_Ref, py_Type);
/// Get the type of the object.
PK_API py_Type py_typeof(py_Ref self);
/// Check if the object is an instance of the given type.
PK_API bool py_isinstance(py_Ref obj, py_Type type);
/// Check if the derived type is a subclass of the base type.
PK_API bool py_issubclass(py_Type derived, py_Type base);
/// Get type by module and name. e.g. `py_gettype("time", py_name("struct_time"))`.
/// Return `0` if not found.
PK_API py_Type py_gettype(const char* module, py_Name name);
/// Check if the object is an instance of the given type exactly.
/// Raise `TypeError` if the check fails.
PK_API bool py_checktype(py_Ref self, py_Type type) PY_RAISE;
/// Check if the object is an instance of the given type or its subclass.
/// Raise `TypeError` if the check fails.
PK_API bool py_checkinstance(py_Ref self, py_Type type) PY_RAISE;
/// Get the magic method from the given type only.
/// Return `nil` if not found.
PK_API PK_DEPRECATED py_GlobalRef py_tpgetmagic(py_Type type, py_Name name);
/// Search the magic method from the given type to the base type.
/// Return `NULL` if not found.
PK_API py_GlobalRef py_tpfindmagic(py_Type, py_Name name);
/// Search the name from the given type to the base type.
/// Return `NULL` if not found.
PK_API py_ItemRef py_tpfindname(py_Type, py_Name name);
/// Get the base type of the given type.
PK_API py_Type py_tpbase(py_Type type);
/// Get the type object of the given type.
PK_API py_GlobalRef py_tpobject(py_Type type);
/// Get the type name.
PK_API const char* py_tpname(py_Type type);
/// Disable the type for subclassing.
PK_API void py_tpsetfinal(py_Type type);
/// Set attribute hooks for the given type.
PK_API void py_tphookattributes(py_Type type,
                                bool (*getattribute)(py_Ref self, py_Name name) PY_RAISE PY_RETURN,
                                bool (*setattribute)(py_Ref self, py_Name name, py_Ref val)
                                    PY_RAISE PY_RETURN,
                                bool (*delattribute)(py_Ref self, py_Name name) PY_RAISE,
                                bool (*getunboundmethod)(py_Ref self, py_Name name) PY_RETURN);

#define py_isint(self) py_istype(self, tp_int)
#define py_isfloat(self) py_istype(self, tp_float)
#define py_isbool(self) py_istype(self, tp_bool)
#define py_isstr(self) py_istype(self, tp_str)
#define py_islist(self) py_istype(self, tp_list)
#define py_istuple(self) py_istype(self, tp_tuple)
#define py_isdict(self) py_istype(self, tp_dict)
#define py_isnil(self) py_istype(self, 0)
#define py_isnone(self) py_istype(self, tp_NoneType)

#define py_checkint(self) py_checktype(self, tp_int)
#define py_checkfloat(self) py_checktype(self, tp_float)
#define py_checkbool(self) py_checktype(self, tp_bool)
#define py_checkstr(self) py_checktype(self, tp_str)

/************* Inspection *************/

/// Get the current `Callable` object on the stack of the most recent vectorcall.
/// Return `NULL` if not available.
/// NOTE: This function should be placed at the beginning of your bindings or you will get wrong result.
PK_API py_StackRef py_inspect_currentfunction();
/// Get the current `module` object where the code is executed.
/// Return `NULL` if not available.
PK_API py_GlobalRef py_inspect_currentmodule();
/// Get the current frame object.
/// Return `NULL` if not available.
PK_API py_Frame* py_inspect_currentframe();
/// Python equivalent to `globals()`.
PK_API void py_newglobals(py_OutRef);
/// Python equivalent to `locals()`.
PK_API void py_newlocals(py_OutRef);

/************* Dict & Slots *************/

/// Get the i-th register.
/// All registers are located in a contiguous memory.
PK_API py_GlobalRef py_getreg(int i);
/// Set the i-th register.
PK_API void py_setreg(int i, py_Ref val);
/// Get the last return value.
/// Please note that `py_retval()` cannot be used as input argument.
PK_API py_GlobalRef py_retval();

#define py_r0() py_getreg(0)
#define py_r1() py_getreg(1)
#define py_r2() py_getreg(2)
#define py_r3() py_getreg(3)
#define py_r4() py_getreg(4)
#define py_r5() py_getreg(5)
#define py_r6() py_getreg(6)
#define py_r7() py_getreg(7)

#define py_tmpr0() py_getreg(8)
#define py_tmpr1() py_getreg(9)
#define py_tmpr2() py_getreg(10)
#define py_tmpr3() py_getreg(11)
#define py_sysr0() py_getreg(12)    // for debugger
#define py_sysr1() py_getreg(13)    // for pybind11

/// Get an item from the object's `__dict__`.
/// Return `NULL` if not found.
PK_API py_ItemRef py_getdict(py_Ref self, py_Name name);
/// Set an item to the object's `__dict__`.
PK_API void py_setdict(py_Ref self, py_Name name, py_Ref val);
/* MANTLE #153: admitted NameDict insertion. Existing-key replacement cannot
 * allocate. New-key growth is failure-atomic and remains charged to the table's
 * exact original quota generation. False denotes allocation-terminal failure;
 * legacy py_setdict retains its non-denying behavior. */
PK_API bool py_trysetdict(py_Ref self, py_Name name, py_Ref val);
/// Delete an item from the object's `__dict__`.
/// Return `true` if the deletion is successful.
PK_API bool py_deldict(py_Ref self, py_Name name);
/// Prepare an insertion to the object's `__dict__`.
PK_API py_ItemRef py_emplacedict(py_Ref self, py_Name name);
/* Output-preserving admitted counterpart to py_emplacedict. */
PK_API bool py_tryemplacedict(py_Ref self, py_Name name, py_ItemRef* out);
/// Apply a function to all items in the object's `__dict__`.
/// Return `true` if the function is successful for all items.
/// NOTE: Be careful if `f` modifies the object's `__dict__`.
PK_API bool
    py_applydict(py_Ref self, bool (*f)(py_Name name, py_Ref val, void* ctx), void* ctx) PY_RAISE;
/// Clear the object's `__dict__`. This function is dangerous.
PK_API void py_cleardict(py_Ref self);
/// Get the i-th slot of the object.
/// The object must have slots and `i` must be in valid range.
PK_API py_ObjectRef py_getslot(py_Ref self, int i);
/// Set the i-th slot of the object.
PK_API void py_setslot(py_Ref self, int i, py_Ref val);
/// Get variable in the `builtins` module.
PK_API py_ItemRef py_getbuiltin(py_Name name);
/// Get variable in the `__main__` module.
PK_API py_ItemRef py_getglobal(py_Name name);
/// Set variable in the `__main__` module.
PK_API void py_setglobal(py_Name name, py_Ref val);

/************* Stack Ops *************/

/// Get the i-th object from the top of the stack.
/// `i` should be negative, e.g. (-1) means TOS.
PK_API py_StackRef py_peek(int i);
/// Push the object to the stack.
PK_API void py_push(py_Ref src);
/// Push a `nil` object to the stack.
PK_API void py_pushnil();
/// Push a `None` object to the stack.
PK_API void py_pushnone();
/// Push a `py_Name` to the stack. This is used for keyword arguments.
PK_API void py_pushname(py_Name name);
/// Pop an object from the stack.
PK_API void py_pop();
/// Shrink the stack by n.
PK_API void py_shrink(int n);
/// Get a temporary variable from the stack.
PK_API py_StackRef py_pushtmp();
/// Get the unbound method of the object.
/// Assume the object is located at the top of the stack.
/// If return true:  `[self] -> [unbound, self]`.
/// If return false: `[self] -> [self]` (no change).
PK_API bool py_pushmethod(py_Name name);
/// Evaluate an expression and push the result to the stack.
/// This function is used for testing.
PK_API bool py_pusheval(const char* expr, py_GlobalRef module) PY_RAISE;
/// Call a callable object via pocketpy's calling convention.
/// You need to prepare the stack using the following format:
/// `callable, self/nil, arg1, arg2, ..., k1, v1, k2, v2, ...`.
/// `argc` is the number of positional arguments excluding `self`.
/// `kwargc` is the number of keyword arguments.
/// The result will be set to `py_retval()`.
/// The stack size will be reduced by `2 + argc + kwargc * 2`.
PK_API bool py_vectorcall(uint16_t argc, uint16_t kwargc) PY_RAISE PY_RETURN;
/// Call a function.
/// It prepares the stack and then performs a `vectorcall(argc, 0, false)`.
/// The result will be set to `py_retval()`.
/// The stack remains unchanged if successful.
PK_API bool py_call(py_Ref f, int argc, py_Ref argv) PY_RAISE PY_RETURN;
/// Call a type to create a new instance.
PK_API bool py_tpcall(py_Type type, int argc, py_Ref argv) PY_RAISE PY_RETURN;

#ifndef NDEBUG
/// Call a `py_CFunction` in a safe way.
/// This function does extra checks to help you debug `py_CFunction`.
PK_API bool py_callcfunc(py_CFunction f, int argc, py_Ref argv) PY_RAISE PY_RETURN;
#else
#define py_callcfunc(f, argc, argv) (f((argc), (argv)))
#endif

#define PY_CHECK_ARGC(n)                                                                           \
    if(argc != n) return TypeError("expected %d arguments, got %d", n, argc)

#define PY_CHECK_ARG_TYPE(i, type)                                                                 \
    if(!py_checktype(py_arg(i), type)) return false

#define py_offset(p, i) ((p) + (i))
#define py_arg(i) (&argv[i])
#define py_assign(dst, src) *(dst) = *(src)

/// Perform a binary operation.
/// The result will be set to `py_retval()`.
/// The stack remains unchanged after the operation.
PK_API bool py_binaryop(py_Ref lhs, py_Ref rhs, py_Name op, py_Name rop) PY_RAISE PY_RETURN;

/************* Python Ops *************/

/// lhs + rhs
PK_API bool py_binaryadd(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs - rhs
PK_API bool py_binarysub(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs * rhs
PK_API bool py_binarymul(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs / rhs
PK_API bool py_binarytruediv(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs // rhs
PK_API bool py_binaryfloordiv(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs % rhs
PK_API bool py_binarymod(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs ** rhs
PK_API bool py_binarypow(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs << rhs
PK_API bool py_binarylshift(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs >> rhs
PK_API bool py_binaryrshift(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs & rhs
PK_API bool py_binaryand(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs | rhs
PK_API bool py_binaryor(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs ^ rhs
PK_API bool py_binaryxor(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs @ rhs
PK_API bool py_binarymatmul(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs == rhs
PK_API bool py_eq(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs != rhs
PK_API bool py_ne(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs < rhs
PK_API bool py_lt(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs <= rhs
PK_API bool py_le(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs > rhs
PK_API bool py_gt(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;
/// lhs >= rhs
PK_API bool py_ge(py_Ref lhs, py_Ref rhs) PY_RAISE PY_RETURN;

/// Python equivalent to `lhs is rhs`.
PK_API bool py_isidentical(py_Ref, py_Ref);
/// Python equivalent to `bool(val)`.
/// 1: true, 0: false, -1: error
PK_API int py_bool(py_Ref val) PY_RAISE;
/// Compare two objects.
/// 1: lhs == rhs, 0: lhs != rhs, -1: error
PK_API int py_equal(py_Ref lhs, py_Ref rhs) PY_RAISE;
/// Compare two objects.
/// 1: lhs < rhs, 0: lhs >= rhs, -1: error
PK_API int py_less(py_Ref lhs, py_Ref rhs) PY_RAISE;
/// Python equivalent to `callable(val)`.
PK_API bool py_callable(py_Ref val);
/// Get the hash value of the object.
PK_API bool py_hash(py_Ref, py_i64* out) PY_RAISE;
/// Get the iterator of the object.
PK_API bool py_iter(py_Ref) PY_RAISE PY_RETURN;
/// Get the next element from the iterator.
/// 1: success, 0: StopIteration, -1: error
PK_API int py_next(py_Ref) PY_RAISE PY_RETURN;
/// Python equivalent to `str(val)`.
PK_API bool py_str(py_Ref val) PY_RAISE PY_RETURN;
/// Python equivalent to `repr(val)`.
PK_API bool py_repr(py_Ref val) PY_RAISE PY_RETURN;
/// Python equivalent to `len(val)`.
PK_API bool py_len(py_Ref val) PY_RAISE PY_RETURN;

/// Python equivalent to `getattr(self, name)`.
PK_API bool py_getattr(py_Ref self, py_Name name) PY_RAISE PY_RETURN;
/// Python equivalent to `setattr(self, name, val)`.
PK_API bool py_setattr(py_Ref self, py_Name name, py_Ref val) PY_RAISE;
/// Python equivalent to `delattr(self, name)`.
PK_API bool py_delattr(py_Ref self, py_Name name) PY_RAISE;
/// Python equivalent to `self[key]`.
PK_API bool py_getitem(py_Ref self, py_Ref key) PY_RAISE PY_RETURN;
/// Python equivalent to `self[key] = val`.
PK_API bool py_setitem(py_Ref self, py_Ref key, py_Ref val) PY_RAISE;
/// Python equivalent to `del self[key]`.
PK_API bool py_delitem(py_Ref self, py_Ref key) PY_RAISE;

/************* Module System *************/

/// Get a module by path.
PK_API py_GlobalRef py_getmodule(const char* path);
/// Create a new module.
PK_API py_GlobalRef py_newmodule(const char* path);
// MANTLE PATCH #156: collectible host module, not entered into the import registry.
// The returned stable cell belongs to the module; the caller must root the value.
PK_API py_Ref py_newmodule_unregistered(const char* path);
// MANTLE PATCH #153: fallible, unpublished host-module construction.  On false,
// `out` is untouched and the exact allocation owner has a terminal record; no
// import-registry node or partially initialized module is published.
PK_API bool py_trynewmodule_unregistered(const char* path, py_OutRef out);
/// Reload an existing module.
PK_API bool py_importlib_reload(py_Ref module) PY_RAISE PY_RETURN;
/// Import a module.
/// The result will be set to `py_retval()`.
/// -1: error, 0: not found, 1: success
PK_API int py_import(const char* path) PY_RAISE PY_RETURN;

/************* PyException *************/

/// Check if there is an unhandled exception.
PK_API bool py_checkexc();

/* MANTLE #153: host-only unwind token for a terminal raised by a nested
 * interpreter invocation after the host restores a different current quota
 * cell. This is not fatal authority: the fixed exact-owner allocation record
 * remains authoritative and the host must latch only after validating it.
 * Latching and clearing are allocation-free. */
PK_API void py_mantle_latch_nested_terminal(void);
PK_API bool py_mantle_nested_terminal_pending(void);
PK_API void py_mantle_clear_nested_terminal(void);
/// Check if the unhandled exception is an instance of the given type.
/// If match, the exception will be stored in `py_retval()`.
PK_API bool py_matchexc(py_Type type) PY_RETURN;
/// Clear the unhandled exception.
/// @param p0 the unwinding point. Use `NULL` if not needed.
PK_API void py_clearexc(py_StackRef p0);
/// Print the unhandled exception.
PK_API void py_printexc();
/// Format the unhandled exception and return a null-terminated string.
/// NULL means either no exception or a recorded allocation terminal; callers
/// must consume that terminal before attempting another formatter allocation.
/// The returned string should be freed by the caller.
PK_API char* py_formatexc();
/// Raise an exception by type and message. Always return false.
PK_API bool py_exception(py_Type type, const char* fmt, ...) PY_RAISE;
/// Raise an exception object. Always return false.
PK_API bool py_raise(py_Ref) PY_RAISE;

#define NameError(n) py_exception(tp_NameError, "name '%n' is not defined", (n))
#define TypeError(...) py_exception(tp_TypeError, __VA_ARGS__)
#define RuntimeError(...) py_exception(tp_RuntimeError, __VA_ARGS__)
#define TimeoutError(...) py_exception(tp_TimeoutError, __VA_ARGS__)
#define OSError(...) py_exception(tp_OSError, __VA_ARGS__)
#define ValueError(...) py_exception(tp_ValueError, __VA_ARGS__)
#define IndexError(...) py_exception(tp_IndexError, __VA_ARGS__)
#define ImportError(...) py_exception(tp_ImportError, __VA_ARGS__)
#define ZeroDivisionError(...) py_exception(tp_ZeroDivisionError, __VA_ARGS__)
#define AttributeError(self, n)                                                                    \
    py_exception(tp_AttributeError, "'%t' object has no attribute '%n'", (self)->type, (n))
#define UnboundLocalError(n)                                                                       \
    py_exception(tp_UnboundLocalError,                                                             \
                 "cannot access local variable '%n' where it is not associated with a value",      \
                 (n))

PK_API bool KeyError(py_Ref key) PY_RAISE;
PK_API bool StopIteration() PY_RAISE;

/************* Debugger *************/

#if PK_ENABLE_OS
PK_API void py_debugger_waitforattach(const char* hostname, unsigned short port);
PK_API int py_debugger_status();
PK_API void py_debugger_exceptionbreakpoint(py_Ref exc);
PK_API void py_debugger_exit(int code);
#else
#define py_debugger_waitforattach(hostname, port)
#define py_debugger_status() 0
#define py_debugger_exceptionbreakpoint(exc)
#define py_debugger_exit(code)
#endif

/************* PyTuple *************/

/// Create a `tuple` with `n` UNINITIALIZED elements.
/// You should initialize all elements before using it.
PK_API py_ObjectRef py_newtuple(py_OutRef, int n);
/* MANTLE #153: admitted compound-literal constructors. These are additive:
 * legacy constructors retain their upstream aborting behavior. On false the
 * output is untouched and the fixed allocation terminal is recorded. */
PK_API bool py_trynewtuple(py_OutRef out, int n, py_ObjectRef* out_items);
PK_API py_ObjectRef py_tuple_data(py_Ref self);
PK_API py_ObjectRef py_tuple_getitem(py_Ref self, int i);
PK_API void py_tuple_setitem(py_Ref self, int i, py_Ref val);
PK_API int py_tuple_len(py_Ref self);

/************* PyList *************/

/// Create an empty `list`.
PK_API void py_newlist(py_OutRef);
/// Create a `list` with `n` UNINITIALIZED elements.
/// You should initialize all elements before using it.
PK_API void py_newlistn(py_OutRef, int n);
PK_API bool py_trynewlistn(py_OutRef out, int n, py_ItemRef* out_items);
/* Admitted, failure-atomic single-item growth for script/runtime paths.
 * The legacy py_list_append/emplace APIs intentionally remain infallible for
 * host/bootstrap compatibility. */
PK_API bool py_trylist_append(py_Ref self, py_Ref val);
PK_API py_ItemRef py_list_data(py_Ref self);
PK_API py_ItemRef py_list_getitem(py_Ref self, int i);
PK_API void py_list_setitem(py_Ref self, int i, py_Ref val);
PK_API void py_list_delitem(py_Ref self, int i);
PK_API int py_list_len(py_Ref self);
PK_API void py_list_swap(py_Ref self, int i, int j);
PK_API void py_list_append(py_Ref self, py_Ref val);
PK_API py_ItemRef py_list_emplace(py_Ref self);
PK_API void py_list_clear(py_Ref self);
PK_API void py_list_insert(py_Ref self, int i, py_Ref val);

/************* PyDict *************/

/// Create an empty `dict`.
PK_API void py_newdict(py_OutRef);
PK_API bool py_trynewdict(py_OutRef out);
/// -1: error, 0: not found, 1: found
PK_API int py_dict_getitem(py_Ref self, py_Ref key) PY_RAISE PY_RETURN;
/// true: success, false: error
PK_API bool py_dict_setitem(py_Ref self, py_Ref key, py_Ref val) PY_RAISE;
/* MANTLE #153: admitted, failure-atomic dict insertion for explicitly
 * converted callers. Existing-key replacement cannot allocate. New-key
 * growth preserves the original dict on allocation-terminal failure. */
PK_API bool py_trydict_setitem(py_Ref self, py_Ref key, py_Ref val) PY_RAISE;
/// -1: error, 0: not found, 1: found (and deleted)
PK_API int py_dict_delitem(py_Ref self, py_Ref key) PY_RAISE;

/// -1: error, 0: not found, 1: found
PK_API int py_dict_getitem_by_str(py_Ref self, const char* key) PY_RAISE PY_RETURN;
/// -1: error, 0: not found, 1: found
PK_API int py_dict_getitem_by_int(py_Ref self, py_i64 key) PY_RAISE PY_RETURN;
/// true: success, false: error
PK_API bool py_dict_setitem_by_str(py_Ref self, const char* key, py_Ref val) PY_RAISE;
/// true: success, false: error
PK_API bool py_dict_setitem_by_int(py_Ref self, py_i64 key, py_Ref val) PY_RAISE;
/// -1: error, 0: not found, 1: found (and deleted)
PK_API int py_dict_delitem_by_str(py_Ref self, const char* key) PY_RAISE;
/// -1: error, 0: not found, 1: found (and deleted)
PK_API int py_dict_delitem_by_int(py_Ref self, py_i64 key) PY_RAISE;

/* MANTLE #153: private root tables ONLY. Start with an empty exact dict and use
 * this pair exclusively: all keys are exact ints with canonical cached hashes,
 * independent of mutable Python int methods. Set returns false after recording
 * an allocation fatal when growth is denied and leaves the table unchanged.
 * Discard uses only existing storage, never dispatches/allocates/frees,
 * and does not access the current VM, retval or exception. Game-thread only;
 * no iteration/interior references may be held across mutation. Not general dict
 * semantics. Invalid object type returns false/-1 without raising. */
PK_API bool py_mantle_root_set(py_Ref self, py_i64 key, py_Ref val);
PK_API int py_mantle_root_discard(py_Ref self, py_i64 key);
/* Diagnostics-only exact admitted allocation marker. `occurrence` counts
 * matching sites; taking hits always disarms the marker. */
enum {
    MantlePocketPy_ContainerFailNone = 0,
    MantlePocketPy_ContainerFailObject = 1,
    MantlePocketPy_ContainerFailDictStorage = 2,
    MantlePocketPy_ContainerFailListStorage = 3,
    /* Collector-only reusable root/work vector; never script-visible. */
    MantlePocketPy_ContainerFailGcRootVector = 4,
    /* OP_ADD_CLASS_ANNOTATION's staged object, long key and Dict backing. */
    MantlePocketPy_ContainerFailClassAnnotationObject = 5,
    MantlePocketPy_ContainerFailClassAnnotationKey = 6,
    MantlePocketPy_ContainerFailClassAnnotationDictStorage = 7,
    /* Normal/generator call staging's raw metadata and long keyword key. */
    MantlePocketPy_ContainerFailCallMetadata = 8,
    MantlePocketPy_ContainerFailCallKey = 9,
    /* Narrow constructor-tail allocation seams.  These are intentionally
     * separate from the generic object marker so each public result can be
     * denied after all of its caller setup and before publication. */
    MantlePocketPy_ContainerFailDictItemsNextTuple = 10,
    MantlePocketPy_ContainerFailBaseExceptionArgsTuple = 11,
    MantlePocketPy_ContainerFailMathModfTuple = 12,
    MantlePocketPy_ContainerFailEnumObject = 13,
    MantlePocketPy_ContainerFailRandomObject = 14,
    /* dict.popitem stages this pair before it is allowed to mutate the
     * dictionary, so a denied result allocation is retryable. */
    MantlePocketPy_ContainerFailDictPopitemTuple = 15,
    /* inspect._signature_data stages its outer result and every entry before
     * publishing the complete nested tuple. */
    MantlePocketPy_ContainerFailInspectSignatureOuterTuple = 16,
    MantlePocketPy_ContainerFailInspectSignatureEntryTuple = 17,
    MantlePocketPy_ContainerFailInspectSignatureTxnRecords = 18
};
/* Deterministic one-shot ordinary-exception allocation terminals.  These
 * diagnostics cover the message buffer, BaseException object, traceback
 * vector reservation and traceback function-name ownership independently. */
enum {
    MantlePocketPy_ExceptionFailNone = 0,
    MantlePocketPy_ExceptionFailMessage = 1,
    MantlePocketPy_ExceptionFailObject = 2,
    MantlePocketPy_ExceptionFailTracebackVector = 3,
    MantlePocketPy_ExceptionFailTracebackName = 4
};
/* MANTLE PATCH #153: these are production call-site markers, passed to the
 * unconditionally compiled pk__try_name_key_txn, so they must be declared
 * outside the diagnostics guard -- Shipping defines
 * MANTLE_POCKETPY_ROOT_DIAGNOSTICS=0 and otherwise fails to compile. */
/* Transient py_Name-to-string conversion denial markers.  These paths never
 * populate CachedNames for script-owned names. */
enum {
    MantlePocketPy_NameValueFailNone = 0,
    MantlePocketPy_NameValueFailTypeName = 1,
    MantlePocketPy_NameValueFailEnumWrapperField = 2,
    MantlePocketPy_NameValueFailGetattrFallback = 3,
    MantlePocketPy_NameValueFailNameDictItem = 4,
    MantlePocketPy_NameValueFailFrameLocalGet = 5,
    MantlePocketPy_NameValueFailFrameLocalSet = 6,
    MantlePocketPy_NameValueFailFrameLocalDel = 7,
    MantlePocketPy_NameValueFailFrameGlobalGet = 8,
    MantlePocketPy_NameValueFailFrameGlobalSet = 9,
    MantlePocketPy_NameValueFailFrameGlobalDel = 10,
    MantlePocketPy_NameValueFailInspectSignatureData = 11
};
#if MANTLE_POCKETPY_ROOT_DIAGNOSTICS
PK_API bool py_mantle_root_storage_test(void);
/* Read-only predicate used to place a deterministic denial at the next real
 * permanent-root storage growth/rehash. */
/* 0 = no allocation, 1 = entry-vector replacement, 2 = table rehash. */
PK_API int py_mantle_root_next_insert_growth_kind(py_Ref self);
/* Read-only predicate for arranging a deterministic real NameDict growth at a
 * specific host publication. `name` must be absent. */
PK_API bool py_mantle_namedict_next_insert_grows(py_Ref self, py_Name name);
/* Bounded native dictionary-storage regressions; no script-visible API. */
PK_API bool py_mantle_dict_storage_test(void);
PK_API bool py_mantle_dict_storage_alias_test(void);
PK_API bool py_mantle_dict_storage_owner_test(int owner_a, int owner_b,
    bool (*check)(size_t a, size_t b, void* context), void* context);
/* Tiny fixed-buffer coverage for the internal fallible c11_sbuf contract. */
PK_API bool py_mantle_sbuf_storage_test(void);
PK_API size_t py_mantle_sbuf_initial_bytes(void);
/* Diagnostics-only route to the original PocketPy print implementation after
 * Mantle replaces the public builtin. Bind only inside a disposable test module. */
PK_API bool py_mantle_builtin_print_for_test(int argc, py_Ref argv);
/* Managed-heap admitted arena/large-object/NameDict prerequisite; no legacy
 * production constructor is switched. */
PK_API bool py_mantle_managed_heap_storage_test(void);
/* NameDict initial/rehash denial plus unpublished object discard ownership. */
PK_API bool py_mantle_namedict_storage_test(unsigned int* failures);
/* Arm a one-shot exact module-global store denial only when `name` is absent
 * and its insertion requires a real NameDict growth. Passing NULL disarms it.
 * Diagnostics-only; production stores always use their ordinary allocator. */
PK_API bool py_mantle_global_store_arm_production_denial(py_Ref module,
                                                          py_Name name);
/* Compile dynamic source and execute it with module globals and nil locals.
 * This diagnostics-only helper reaches OP_STORE_NAME's module-global branch
 * without restoring the sandbox-disabled Python exec builtin. */
PK_API bool py_mantle_global_store_exec_dynamic_for_test(
    py_Ref module, const char* source);
/* Function/closure object, snapshot and self-binding transaction coverage. */
PK_API bool py_mantle_closure_storage_test(unsigned int* failures);
/* Compound literal allocation/publication and dict callback-root audit. */
PK_API bool py_mantle_container_bytecode_storage_test(unsigned int* failures);
PK_API bool py_mantle_container_test_arm_fail_site(int site, int occurrence);
PK_API int py_mantle_container_test_take_fail_hits(int site);
/* Diagnostics-only direct probes for the simple constructor-tail seams.
 * `terminal` arms exactly one site after module/action setup. */
PK_API bool py_mantle_simple_constructor_probe(py_Ref module, int mode,
                                               bool terminal,
                                               bool* out_exact_rollback,
                                               bool* out_no_exception);
/* Diagnostics-only dict.popitem probe.  It compiles/setup before arming and
 * checks both denial atomicity and a healthy exact-pair retry-equivalent. */
PK_API bool py_mantle_dict_popitem_probe(py_Ref module, bool terminal,
                                         bool* out_exact_rollback,
                                         bool* out_no_exception,
                                         bool* out_semantics);
/* Diagnostics-only inspect._signature_data probe modes.  The first/later
 * entry modes share the nested-entry marker but arm occurrence one/two. */
enum {
    MantlePocketPy_SignatureDataProbeFailOuterTuple = 1,
    MantlePocketPy_SignatureDataProbeFailFirstEntryTuple = 2,
    MantlePocketPy_SignatureDataProbeFailLaterEntryTuple = 3,
    MantlePocketPy_SignatureDataProbeFailTxnRecords = 4,
    MantlePocketPy_SignatureDataProbeFailLongName = 5
};
PK_API bool py_mantle_signature_data_probe(py_Ref module, int mode,
                                           bool terminal,
                                           bool* out_exact_rollback,
                                           bool* out_no_exception,
                                           bool* out_semantics);
/* Diagnostics-only probes for Random.choices and function metadata.  The
 * choices modes cover its cumulative scratch allocation, result object, and
 * result backing respectively; metadata modes cover __name__ and __doc__. */
enum {
    MantlePocketPy_RandomChoicesProbeFailCumulative = 1,
    MantlePocketPy_RandomChoicesProbeFailListObject = 2,
    MantlePocketPy_RandomChoicesProbeFailListBacking = 3
};
PK_API bool py_mantle_random_choices_probe(py_Ref module, int mode,
                                           bool terminal,
                                           bool* out_exact_rollback,
                                           bool* out_no_exception,
                                           bool* out_semantics);
enum {
    MantlePocketPy_FunctionMetadataProbeFailName = 1,
    MantlePocketPy_FunctionMetadataProbeFailDoc = 2
};
PK_API bool py_mantle_function_metadata_probe(py_Ref module, int mode,
                                               bool terminal,
                                               bool* out_exact_rollback,
                                               bool* out_no_exception,
                                               bool* out_semantics);
/* Diagnostics-only direct probe for vec2.smooth_damp's staged tuple result. */
PK_API bool py_mantle_vec2_smoothdamp_probe(py_Ref module, bool terminal,
                                            bool* out_exact_rollback,
                                            bool* out_no_exception,
                                            bool* out_semantics);
enum {
    MantlePocketPy_Mat3x3ProbeNew = 1,
    MantlePocketPy_Mat3x3ProbeMatmul = 2,
    MantlePocketPy_Mat3x3ProbeInvert = 3,
    MantlePocketPy_Mat3x3ProbeCopy = 4,
    MantlePocketPy_Mat3x3ProbeInverse = 5,
    MantlePocketPy_Mat3x3ProbeZeros = 6,
    MantlePocketPy_Mat3x3ProbeIdentity = 7,
    MantlePocketPy_Mat3x3ProbeTrs = 8
};
PK_API bool py_mantle_mat3x3_result_probe(py_Ref module, int mode, bool terminal,
                                          bool* out_exact_rollback,
                                          bool* out_no_exception,
                                          bool* out_semantics);
PK_API bool py_mantle_vmath_repr_probe(py_Ref module, bool terminal,
                                       bool* out_exact_rollback,
                                       bool* out_no_exception,
                                       bool* out_semantics);
PK_API bool py_mantle_instance_attr_probe(py_Ref module, bool terminal,
                                          bool* out_exact_rollback,
                                          bool* out_no_exception,
                                          bool* out_semantics);
/* #171: a table minted with no quota cell current (as host/bootstrap code
 * does) and then grown by a store from the running script. `module` is the
 * running script's module; it only roots the instance-mode object. */
enum {
    MantlePocketPy_NativeOwnedInstanceAttr = 1,
    MantlePocketPy_NativeOwnedGlobalStore = 2,
    MantlePocketPy_NativeOwnedImportStar = 3,
    MantlePocketPy_NativeOwnedEndClass = 4
};
PK_API bool py_mantle_native_owned_table_probe(py_Ref module, int mode,
                                               bool terminal,
                                               bool* out_exact_rollback,
                                               bool* out_no_exception,
                                               bool* out_semantics,
                                               bool* out_charged);
PK_API bool py_mantle_name_value_test_arm_fail_site(int site, int occurrence);
PK_API int py_mantle_name_value_test_take_fail_hits(int site);
/* Diagnostics-only dynamic-frame probe for the six dict-backed name paths.
 * `terminal` arms its exact mode after all source/name setup is complete. */
PK_API bool py_mantle_frame_name_value_probe(py_Ref module, int mode,
                                              bool terminal,
                                              bool* out_exact_rollback,
                                              bool* out_no_exception);
/* Fast-locals snapshot allocation markers.  These are diagnostics-only and
 * never participate in normal dictionary construction or mutation. */
enum {
    MantlePocketPy_FastLocalsFailNone = 0,
    MantlePocketPy_FastLocalsFailDictObject = 1,
    MantlePocketPy_FastLocalsFailInitialBacking = 2,
    MantlePocketPy_FastLocalsFailLongKeyMetadata = 3,
    MantlePocketPy_FastLocalsFailLongManagedKey = 4,
    MantlePocketPy_FastLocalsFailEntriesGrowth = 5,
    MantlePocketPy_FastLocalsFailRehash = 6
};
PK_API bool py_mantle_fastlocals_test_arm_fail_site(int site, int occurrence);
PK_API int py_mantle_fastlocals_test_take_fail_hits(int site);
/* Collector root/work-vector admission denial/retry/accounting regression. */
PK_API bool py_mantle_gc_root_storage_test(void);
/* BEGIN_CLASS type-table/object/NameDict publication plus reload identity. */
PK_API bool py_mantle_class_storage_test(py_Ref module, unsigned int* failures);
/* One-shot exact-owner BEGIN and post-exposure final-binding denial seams. */
PK_API void py_mantle_class_set_production_denial(bool enabled);
PK_API void py_mantle_class_set_binding_denial(bool enabled);
PK_API bool py_mantle_class_install_on_end_test_base(py_Ref module, bool terminal);
#if MANTLE_POCKETPY_ROOT_DIAGNOSTICS
/* One-shot registered-import allocation seam. Stages: 1 initial module
 * NameDict backing, 2 module object, 3 package path, 4 full path, 5 __name__,
 * 6 __package__, 7 registry node. The caller owns/restores the cell limit. */
PK_API void py_mantle_import_set_production_denial(int stage);
PK_API bool py_mantle_import_production_denial_pending(void);
// Test-only production seam for py_trynewmodule_unregistered.  Stages are
// 1 module NameDict, 2 module object, 3 package path, 4 full path,
// 5 __name__ value, 6 __package__ value; any other value disarms it.  The
// host's later __mantle_instance_id__ binding is stage 7 in its C++ seam.
PK_API void py_mantle_host_module_set_production_denial(int stage);
#endif
/* Managed/c11 string and public object boundary/publication cleanup. */
PK_API bool py_mantle_managed_string_storage_test(unsigned int* failures);
/* str.join iterator-root cleanup and ctor/growth/publication denial stages. */
PK_API bool py_mantle_join_storage_test(unsigned int* failures);
/* Detached replace/slice exact allocation and managed publication cleanup. */
PK_API bool py_mantle_detached_string_storage_test(unsigned int* failures);
/* str.split/splitlines span/list/string/object denial and deferred-GC stages. */
PK_API bool py_mantle_split_storage_test(unsigned int* failures);
/* bytes payload/object denial, publication atomicity and malformed decode. */
PK_API bool py_mantle_bytes_storage_test(unsigned int* failures);
/* SourceData object/filename/normalization/line-plan construction and public
 * compile publication denial. */
PK_API bool py_mantle_source_data_storage_test(unsigned int* failures);
/* Public compile lexer token/indent queues and token-owned string payloads. */
PK_API bool py_mantle_lexer_storage_test(unsigned int* failures);
/* One-shot exact-owner denial at the production lexer's first allocation. */
PK_API void py_mantle_lexer_set_production_denial(bool enabled);
/* Public compiler/codegen vectors, CodeObject names and FuncDecl ownership. */
PK_API bool py_mantle_compiler_storage_test(unsigned int* failures);
/* Public compiler global/inverse-name/keyword maps and paired publication. */
PK_API bool py_mantle_compiler_map_storage_test(unsigned int* failures);
/* Compiler-only fallible process-global name interning and ownership. */
PK_API bool py_mantle_compiler_name_storage_test(unsigned int* failures);
/* Produce a process-globally absent source name and atomically arm its denial. */
PK_API bool py_mantle_compiler_prepare_name_terminal_for_test(char* source,
                                                               int capacity);
/* One-shot exact-owner denial at the production compiler's first name intern. */
PK_API void py_mantle_compiler_set_production_denial(bool enabled);
/* Generator construction/resume allocation stages; diagnostics only. */
enum {
    MantlePocketPy_GeneratorFailNone = 0,
    MantlePocketPy_GeneratorFailObject = 1,
    MantlePocketPy_GeneratorFailInitialList = 2,
    MantlePocketPy_GeneratorFailFrame = 3,
    MantlePocketPy_GeneratorFailYieldSnapshot = 4,
    MantlePocketPy_GeneratorFailExceptionFrame = 5
};
PK_API bool py_mantle_generator_test_arm_fail_site(int site, int occurrence);
PK_API int py_mantle_generator_test_take_fail_hits(int site);
PK_API int py_mantle_generator_frame_overflow_bytes_for_test(void);
PK_API int py_mantle_generator_frame_block_bytes_for_test(void);
PK_API int py_mantle_frame_pool_free_count_for_test(void);
PK_API int py_mantle_frame_pool_block_count_for_test(void);
PK_API bool py_mantle_frame_storage_test(py_Ref module, unsigned int* failures);
PK_API bool py_mantle_frame_exec_terminal_test(py_Ref module, bool is_dynamic);
/* One-shot malloc denial at the exact built-in StopIteration transaction.
 * Diagnostics-only; production always uses the ordinary admitted allocator. */
PK_API bool py_mantle_stop_iteration_test_arm_denial(int occurrence);
PK_API int py_mantle_stop_iteration_test_take_denial_hits(void);
PK_API int py_mantle_stop_iteration_test_take_inert_generator_hits(void);
PK_API bool py_mantle_exception_test_arm_fail_site(int site, int occurrence);
PK_API int py_mantle_exception_test_take_fail_hits(int site);
/* One-shot formatter admission denial.  It is diagnostics-only and reaches
 * formatexc_internal before its first checked buffer reservation. */
PK_API bool py_mantle_formatexc_test_arm_denial(int occurrence);
/* Takes the one-shot formatter-denial receipt only for its exact fatal owner.
 * Fixed POD only: stale/sibling terminal processing cannot steal attribution. */
PK_API int py_mantle_formatexc_test_take_denial_receipt(
    int vm, int instance, unsigned long long generation);
PK_API bool py_mantle_formatexc_test_denial_receipt_matches(
    int vm, int instance, unsigned long long generation);
PK_API bool py_mantle_stop_iteration_storage_test(unsigned int* failures);
/* Public parser expression nodes plus expression/call/jump storage. */
PK_API bool py_mantle_parser_storage_test(unsigned int* failures);
/* One-shot exact-owner denial at the production parser's first expression. */
PK_API void py_mantle_parser_set_production_denial(bool enabled);
/* Historical backing-entry count: includes discarded tombstones until root-table
 * compaction.  Retained for storage/compaction diagnostics. */
PK_API int py_mantle_root_entry_count(py_Ref self);
/* Live private-root count: excludes discarded tombstones.  Read-only and
 * diagnostics-only; use this for publication/leak assertions. */
PK_API int py_mantle_root_live_count(py_Ref self);
#endif

/// true: success, false: error
PK_API bool
    py_dict_apply(py_Ref self, bool (*f)(py_Ref key, py_Ref val, void* ctx), void* ctx) PY_RAISE;
/// noexcept
PK_API int py_dict_len(py_Ref self);

/************* PySlice *************/

/// Create an UNINITIALIZED `slice` object.
/// You should use `py_setslot()` to set `start`, `stop`, and `step`.
PK_API py_ObjectRef py_newslice(py_OutRef);
PK_API bool py_trynewslice(py_OutRef out, py_ObjectRef* out_slots);
/// Create a `slice` object from 3 integers.
PK_API void py_newsliceint(py_OutRef out, py_i64 start, py_i64 stop, py_i64 step);

/************* random module *************/
PK_API void py_newRandom(py_OutRef out);
PK_API void py_Random_seed(py_Ref self, py_i64 seed);
PK_API py_f64 py_Random_random(py_Ref self);
PK_API py_f64 py_Random_uniform(py_Ref self, py_f64 a, py_f64 b);
PK_API py_i64 py_Random_randint(py_Ref self, py_i64 a, py_i64 b);

/************* array2d module *************/
PK_API void py_newarray2d(py_OutRef out, int width, int height);
PK_API int py_array2d_getwidth(py_Ref self);
PK_API int py_array2d_getheight(py_Ref self);
PK_API py_ObjectRef py_array2d_getitem(py_Ref self, int x, int y);
PK_API void py_array2d_setitem(py_Ref self, int x, int y, py_Ref val);

/************* vmath module *************/
PK_API void py_newvec2(py_OutRef out, c11_vec2);
PK_API void py_newvec3(py_OutRef out, c11_vec3);
PK_API void py_newvec2i(py_OutRef out, c11_vec2i);
PK_API void py_newvec3i(py_OutRef out, c11_vec3i);
PK_API void py_newvec4i(py_OutRef out, c11_vec4i);
PK_API void py_newcolor32(py_OutRef out, c11_color32);
PK_API c11_mat3x3* py_newmat3x3(py_OutRef out);
PK_API c11_vec2 py_tovec2(py_Ref self);
PK_API c11_vec3 py_tovec3(py_Ref self);
PK_API c11_vec2i py_tovec2i(py_Ref self);
PK_API c11_vec3i py_tovec3i(py_Ref self);
PK_API c11_vec4i py_tovec4i(py_Ref self);
PK_API c11_mat3x3* py_tomat3x3(py_Ref self);
PK_API c11_color32 py_tocolor32(py_Ref self);

/************* json module *************/
/// Python equivalent to `json.dumps(val)`.
PK_API bool py_json_dumps(py_Ref val, int indent) PY_RAISE PY_RETURN;
/// Python equivalent to `json.loads(val)`.
PK_API bool py_json_loads(const char* source) PY_RAISE PY_RETURN;

/************* pickle module *************/
/// Python equivalent to `pickle.dumps(val)`.
PK_API bool py_pickle_dumps(py_Ref val) PY_RAISE PY_RETURN;
/// Python equivalent to `pickle.loads(val)`.
PK_API bool py_pickle_loads(const unsigned char* data, int size) PY_RAISE PY_RETURN;

/************* pkpy module *************/
/// Begin the watchdog with `timeout` in milliseconds.
/// `PK_ENABLE_WATCHDOG` must be defined to `1` to use this feature.
/// You need to call `py_watchdog_end()` later.
/// If `timeout` is reached, `TimeoutError` will be raised.
PK_API void py_watchdog_begin(py_i64 timeout);
/// Reset the watchdog.
PK_API void py_watchdog_end();

PK_API void py_profiler_begin();
PK_API void py_profiler_end();
PK_API void py_profiler_reset();
PK_API char* py_profiler_report();

/************* Others *************/
int64_t time_ns();
int64_t time_monotonic_ns();
py_i64 cpy312__int_floordiv(py_i64 a, py_i64 b);
py_i64 cpy312__int_mod(py_i64 a, py_i64 b);
void cpy312__float_divmod(double vx, double wx, double *floordiv, double *mod);

/// An utility function to read a line from stdin for REPL.
PK_API int py_replinput(char* buf, int max_size);

/// Python favored string formatting.
/// %d: int
/// %i: py_i64 (int64_t)
/// %f: py_f64 (double)
/// %s: const char*
/// %q: c11_sv
/// %v: c11_sv
/// %c: char
/// %p: void*
/// %t: py_Type
/// %n: py_Name

enum py_PredefinedType {
    tp_nil = 0,
    tp_object = 1,
    tp_type,  // py_Type
    tp_int,
    tp_float,
    tp_bool,
    tp_str,
    tp_str_iterator,
    tp_list,            // c11_vector
    tp_tuple,           // N slots
    tp_list_iterator,   // 1 slot
    tp_tuple_iterator,  // 1 slot
    tp_slice,           // 3 slots (start, stop, step)
    tp_range,
    tp_range_iterator,
    tp_module,
    tp_function,
    tp_nativefunc,
    tp_boundmethod,  // 2 slots (self, func)
    tp_super,        // 1 slot + py_Type
    tp_BaseException,
    tp_Exception,
    tp_bytes,
    tp_bytes_iterator,
    tp_namedict,
    tp_locals,
    tp_code,
    tp_dict,
    tp_dict_iterator,  // 1 slot
    tp_property,       // 2 slots (getter + setter)
    tp_star_wrapper,   // 1 slot + int level
    tp_staticmethod,   // 1 slot
    tp_classmethod,    // 1 slot
    tp_NoneType,
    tp_NotImplementedType,
    tp_ellipsis,
    tp_generator,
    /* builtin exceptions */
    tp_SystemExit,
    tp_KeyboardInterrupt,
    tp_StopIteration,
    tp_SyntaxError,
    tp_RecursionError,
    tp_OSError,
    tp_PermissionError,
    tp_NotImplementedError,
    tp_TypeError,
    tp_IndexError,
    tp_ValueError,
    tp_RuntimeError,
    tp_TimeoutError,
    tp_ZeroDivisionError,
    tp_NameError,
    tp_UnboundLocalError,
    tp_AttributeError,
    tp_ImportError,
    tp_AssertionError,
    tp_KeyError,
    /* stdc */
    tp_stdc_Memory,
    tp_stdc_Char, tp_stdc_UChar,
    tp_stdc_Short, tp_stdc_UShort,
    tp_stdc_Int, tp_stdc_UInt,
    tp_stdc_Long, tp_stdc_ULong,
    tp_stdc_LongLong, tp_stdc_ULongLong,
    tp_stdc_Float, tp_stdc_Double,
    tp_stdc_Pointer,
    tp_stdc_Bool,
    /* vmath */
    tp_vec2,
    tp_vec3,
    tp_vec2i,
    tp_vec3i,
    tp_vec4i,
    tp_mat3x3,
    tp_color32,
    /* array2d */
    tp_array2d_like,
    tp_array2d_like_iterator,
    tp_array2d,
    tp_array2d_view,
    tp_chunked_array2d,
};

#ifndef PK_IS_AMALGAMATED_C
#ifdef PK_IS_PUBLIC_INCLUDE
typedef struct py_TValue {
    py_Type type;
    bool is_ptr;
    // MANTLE PATCH (#123): nativefunc receiver owner (+instance type, -constructor type, 0=plain).
    // Other types retain their upstream extra semantics; no layout change.
    int extra;

    union {
        int64_t _i64;
        double _f64;
        bool _bool;
        py_CFunction _cfunc;
        void* _obj;
        void* _ptr;
        char _chars[16];
    };
} py_TValue;
#endif
#endif

#ifdef __cplusplus
}
#endif
