/**
 * @file jni.h
 * @brief Position-independent JNI bridge for Android ART VM
 *
 * @details Minimal JNI type definitions and a bridge to attach to the
 * running ART Java VM from position-independent native code.
 *
 * Uses raw JNI function table indices (from the JNI spec) instead of a
 * C struct — avoids off-by-one bugs from placeholder field counts.
 * The JNI table is a flat array of function pointers; indices are stable
 * across all Android versions per the JNI specification.
 *
 * @ingroup kernel_android
 */

#pragma once

#include "core/types/primitives.h"

// =============================================================================
// JNI type definitions
// =============================================================================

using jobject      = PVOID;
using jclass       = jobject;
using jstring      = jobject;
using jmethodID    = PVOID;
using jfieldID     = PVOID;
using jint         = INT32;
using jlong        = INT64;
using jboolean     = UINT8;
using jsize        = INT32;
using jobjectArray = jobject;
using jintArray    = jobject;

union jvalue
{
	jboolean z;
	INT8     b;
	UINT16   c;
	INT16    s;
	jint     i;
	jlong    j;
	float    f;
	double   d;
	jobject  l;
};

// =============================================================================
// JNI function table — accessed via raw indices
// =============================================================================

/// @brief JNIEnv is a double-indirection pointer to the function table.
/// env[0] points to the function table (array of function pointers).
/// Access pattern: ((FnType)((PVOID *)*env)[INDEX])(env, ...)
using JNIEnv = PVOID *;

// =============================================================================
// JNI function table indices (JNI spec — stable across all versions)
// =============================================================================

// clang-format off
constexpr INT32 JNI_IDX_FindClass                  = 6;
constexpr INT32 JNI_IDX_ExceptionOccurred          = 15;
constexpr INT32 JNI_IDX_ExceptionClear             = 17;
constexpr INT32 JNI_IDX_NewGlobalRef               = 21;
constexpr INT32 JNI_IDX_DeleteGlobalRef            = 22;
constexpr INT32 JNI_IDX_DeleteLocalRef             = 23;
constexpr INT32 JNI_IDX_NewObject                  = 28;
constexpr INT32 JNI_IDX_GetObjectClass             = 31;
constexpr INT32 JNI_IDX_GetMethodID                = 33;
constexpr INT32 JNI_IDX_CallObjectMethod           = 34;
constexpr INT32 JNI_IDX_CallBooleanMethod          = 37;
constexpr INT32 JNI_IDX_CallIntMethod              = 49;
constexpr INT32 JNI_IDX_CallVoidMethod             = 61;
constexpr INT32 JNI_IDX_CallVoidMethodA            = 63;
constexpr INT32 JNI_IDX_GetStaticMethodID          = 113;
constexpr INT32 JNI_IDX_CallStaticObjectMethod     = 114;
constexpr INT32 JNI_IDX_CallStaticObjectMethodA    = 116;
constexpr INT32 JNI_IDX_CallStaticIntMethod        = 129;
constexpr INT32 JNI_IDX_CallStaticVoidMethod       = 141;
constexpr INT32 JNI_IDX_GetStaticFieldID           = 144;
constexpr INT32 JNI_IDX_GetStaticObjectField       = 145;
constexpr INT32 JNI_IDX_GetStaticIntField          = 150;
constexpr INT32 JNI_IDX_NewStringUTF               = 167;
constexpr INT32 JNI_IDX_GetArrayLength             = 171;
constexpr INT32 JNI_IDX_GetObjectArrayElement      = 173;
constexpr INT32 JNI_IDX_NewIntArray                = 176;
constexpr INT32 JNI_IDX_GetIntArrayElements        = 187;
constexpr INT32 JNI_IDX_ReleaseIntArrayElements    = 197;
constexpr INT32 JNI_IDX_GetDirectBufferAddress     = 230;
// clang-format on

// =============================================================================
// JNI function pointer types (common signatures)
// =============================================================================

using JniFn_FindClass              = jclass  (*)(PVOID env, const CHAR *name);
using JniFn_ExceptionOccurred      = jobject (*)(PVOID env);
using JniFn_ExceptionClear         = void    (*)(PVOID env);
using JniFn_NewGlobalRef           = jobject (*)(PVOID env, jobject obj);
using JniFn_DeleteGlobalRef        = void    (*)(PVOID env, jobject ref);
using JniFn_DeleteLocalRef         = void    (*)(PVOID env, jobject ref);
using JniFn_GetObjectClass         = jclass  (*)(PVOID env, jobject obj);
using JniFn_GetMethodID            = jmethodID (*)(PVOID env, jclass cls, const CHAR *name, const CHAR *sig);
using JniFn_CallObjectMethod       = jobject (*)(PVOID env, jobject obj, jmethodID mid, ...);
using JniFn_CallIntMethod          = jint    (*)(PVOID env, jobject obj, jmethodID mid, ...);
using JniFn_CallVoidMethod         = void    (*)(PVOID env, jobject obj, jmethodID mid, ...);
using JniFn_CallVoidMethodA        = void    (*)(PVOID env, jobject obj, jmethodID mid, const jvalue *args);
using JniFn_GetStaticMethodID      = jmethodID (*)(PVOID env, jclass cls, const CHAR *name, const CHAR *sig);
using JniFn_CallStaticObjectMethod = jobject (*)(PVOID env, jclass cls, jmethodID mid, ...);
using JniFn_CallStaticObjectMethodA= jobject (*)(PVOID env, jclass cls, jmethodID mid, const jvalue *args);
using JniFn_CallStaticVoidMethod   = void    (*)(PVOID env, jclass cls, jmethodID mid, ...);
using JniFn_NewStringUTF           = jstring (*)(PVOID env, const CHAR *bytes);
using JniFn_GetObjectArrayElement  = jobject (*)(PVOID env, jobjectArray arr, jsize idx);
using JniFn_NewIntArray            = jintArray (*)(PVOID env, jsize len);
using JniFn_GetIntArrayElements    = jint *  (*)(PVOID env, jintArray arr, jboolean *isCopy);
using JniFn_ReleaseIntArrayElements= void    (*)(PVOID env, jintArray arr, jint *elems, jint mode);
using JniFn_GetDirectBufferAddress = PVOID   (*)(PVOID env, jobject buf);

// =============================================================================
// JNI table access macro
// =============================================================================

/// @brief Get a typed function pointer from the JNI function table
/// @param env JNIEnv * (PVOID **)
/// @param TYPE Function pointer type (JniFn_*)
/// @param IDX JNI_IDX_* constant
#define JNI_FN(env, TYPE, IDX) ((TYPE)((PVOID *)*env)[IDX])

// =============================================================================
// JavaVM invocation interface — also raw index access
// =============================================================================

/// @brief JavaVM is a double-indirection pointer to the invocation table
using JavaVM = PVOID *;

constexpr INT32 JVM_IDX_DestroyJavaVM               = 3;
constexpr INT32 JVM_IDX_AttachCurrentThread          = 4;
constexpr INT32 JVM_IDX_DetachCurrentThread          = 5;
constexpr INT32 JVM_IDX_GetEnv                       = 6;
constexpr INT32 JVM_IDX_AttachCurrentThreadAsDaemon  = 7;

using JvmFn_GetEnv                    = jint (*)(PVOID vm, PVOID *penv, jint version);
using JvmFn_AttachCurrentThreadAsDaemon = jint (*)(PVOID vm, PVOID *penv, PVOID args);

#define JVM_FN(vm, TYPE, IDX) ((TYPE)((PVOID *)*vm)[IDX])

// =============================================================================
// Constants
// =============================================================================

constexpr jint JNI_VERSION_1_6 = 0x00010006;
constexpr jint JNI_OK = 0;

/// @brief Signature for JNI_GetCreatedJavaVMs (resolved from libart.so)
using FnGetCreatedJavaVMs = jint (*)(JavaVM **vmBuf, jsize bufLen, jsize *nVMs);

// =============================================================================
// JNI Bridge API
// =============================================================================

/**
 * @brief Attach to the running ART Java VM and get a JNIEnv
 *
 * @param outEnv [out] Receives the JNIEnv pointer on success
 * @param outVm  [out] Receives the JavaVM pointer on success (optional)
 * @return true on success, false on failure
 */
BOOL JniBridgeAttach(JNIEnv **outEnv, JavaVM **outVm);
