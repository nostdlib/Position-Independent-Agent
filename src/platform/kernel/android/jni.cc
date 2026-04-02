/**
 * @file jni.cc
 * @brief JNI bridge implementation — attach to ART from PIC code
 *
 * @details Resolution chain:
 *   1. FindLoadedLibrary("libart.so") → parse /proc/self/maps for base addr
 *   2. ResolveElfSymbol(base, "JNI_GetCreatedJavaVMs") → function pointer
 *   3. JNI_GetCreatedJavaVMs(&vm, 1, &count) → get the running ART VM
 *   4. vm->GetEnv or AttachCurrentThreadAsDaemon → get JNIEnv
 */

#include "platform/kernel/android/jni.h"
#include "platform/kernel/android/dl.h"

BOOL JniBridgeAttach(JNIEnv **outEnv, JavaVM **outVm)
{
	if (outEnv == nullptr)
		return false;

	*outEnv = nullptr;
	if (outVm != nullptr)
		*outVm = nullptr;

	// Step 1: Find libart.so in memory
	auto libartName = "libart.so";
	PVOID artBase = FindLoadedLibrary((const CHAR *)libartName);
	if (artBase == nullptr)
		return false;

	// Step 2: Resolve JNI_GetCreatedJavaVMs from libart.so's ELF exports
	auto symName = "JNI_GetCreatedJavaVMs";
	PVOID symAddr = ResolveElfSymbol(artBase, (const CHAR *)symName);
	if (symAddr == nullptr)
		return false;

	FnGetCreatedJavaVMs getCreatedVMs = (FnGetCreatedJavaVMs)symAddr;

	// Step 3: Get the running Java VM
	JavaVM *vm = nullptr;
	jsize vmCount = 0;
	jint result = getCreatedVMs(&vm, 1, &vmCount);
	if (result != JNI_OK || vmCount == 0 || vm == nullptr)
		return false;

	// Step 4: Try GetEnv first (thread may already be attached)
	JNIEnv *env = nullptr;
	auto getEnv = JVM_FN(vm, JvmFn_GetEnv, JVM_IDX_GetEnv);
	result = getEnv((PVOID)vm, (PVOID *)&env, JNI_VERSION_1_6);
	if (result == JNI_OK && env != nullptr)
	{
		*outEnv = env;
		if (outVm != nullptr)
			*outVm = vm;
		return true;
	}

	// Step 5: Attach current thread as daemon (won't prevent VM shutdown)
	auto attach = JVM_FN(vm, JvmFn_AttachCurrentThreadAsDaemon, JVM_IDX_AttachCurrentThreadAsDaemon);
	result = attach((PVOID)vm, (PVOID *)&env, nullptr);
	if (result != JNI_OK || env == nullptr)
		return false;

	*outEnv = env;
	if (outVm != nullptr)
		*outVm = vm;
	return true;
}
