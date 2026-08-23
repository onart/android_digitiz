#include "platform/ActivityBridge.hpp"

#include <jni.h>

#include <game-activity/GameActivity.h>

#include <digitiz/core/log.hpp>

namespace digitiz::guest {

namespace {

// The render thread is usually already attached, by the text renderer. This
// must not depend on that having happened, and must not detach a thread it did
// not attach.
class ScopedEnv {
public:
    explicit ScopedEnv(JavaVM* vm) : vm_(vm) {
        if (vm_ == nullptr) {
            return;
        }
        if (vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_OK) {
            return;
        }
        if (vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
            owned_ = true;
        } else {
            env_ = nullptr;
        }
    }

    ~ScopedEnv() {
        if (owned_) {
            vm_->DetachCurrentThread();
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    JNIEnv* get() const noexcept { return env_; }

private:
    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool owned_ = false;
};

} // namespace

void flip_orientation(GameActivity* activity) {
    if (activity == nullptr || activity->vm == nullptr) {
        DZ_WARN("orientation: no activity");
        return;
    }

    ScopedEnv scoped(activity->vm);
    JNIEnv* env = scoped.get();
    if (env == nullptr) {
        DZ_WARN("orientation: could not reach the VM");
        return;
    }

    jclass cls = env->GetObjectClass(activity->javaGameActivity);
    jmethodID method = env->GetMethodID(cls, "flipOrientation", "()V");
    if (method == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        DZ_WARN("orientation: MainActivity has no flipOrientation()");
        return;
    }

    env->CallVoidMethod(activity->javaGameActivity, method);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    DZ_INFO("display turned the other way round");
}

} // namespace digitiz::guest
