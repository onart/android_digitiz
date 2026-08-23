#include "platform/ActivityBridge.hpp"

#include <mutex>

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

// Calls one void method on the activity, looked up by name each time. These
// happen when a person taps something, so the lookup cost is irrelevant and
// caching it would mean holding global refs across surface teardowns.
JNIEnv* activity_env(GameActivity* activity, ScopedEnv& scoped) {
    if (activity == nullptr || activity->vm == nullptr) {
        DZ_WARN("activity bridge: no activity");
        return nullptr;
    }
    JNIEnv* env = scoped.get();
    if (env == nullptr) {
        DZ_WARN("activity bridge: could not reach the VM");
    }
    return env;
}

void report_exception(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

// The JNI entry points below are static, so there is nowhere to hang an
// instance. The queue is a file-scope singleton for that reason, and the mutex
// is what makes the hop from the UI thread to the render thread safe.
std::mutex g_inbox_mutex;
std::vector<ButtonEdit> g_edits;
std::vector<ButtonCommand> g_commands;
std::vector<PresetCommand> g_preset_commands;

std::string to_utf8(JNIEnv* env, jstring s) {
    if (s == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars != nullptr ? chars : "");
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(s, chars);
    }
    return out;
}

} // namespace

void flip_orientation(GameActivity* activity) {
    ScopedEnv scoped(activity != nullptr ? activity->vm : nullptr);
    JNIEnv* env = activity_env(activity, scoped);
    if (env == nullptr) {
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
    report_exception(env);
    env->DeleteLocalRef(cls);
    DZ_INFO("display turned the other way round");
}

void show_button_editor(GameActivity* activity, int index, int kind, const std::string& label,
                        int x, int y, int w, int h, int modifiers, const std::string& key) {
    ScopedEnv scoped(activity != nullptr ? activity->vm : nullptr);
    JNIEnv* env = activity_env(activity, scoped);
    if (env == nullptr) {
        return;
    }

    jclass cls = env->GetObjectClass(activity->javaGameActivity);
    jmethodID method =
        env->GetMethodID(cls, "showButtonEditor", "(IILjava/lang/String;IIIIILjava/lang/String;)V");
    if (method == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        DZ_WARN("buttons: MainActivity has no showButtonEditor()");
        return;
    }

    jstring jlabel = env->NewStringUTF(label.c_str());
    jstring jkey = env->NewStringUTF(key.c_str());
    env->CallVoidMethod(activity->javaGameActivity, method, index, kind, jlabel, x, y, w, h,
                        modifiers, jkey);
    report_exception(env);
    env->DeleteLocalRef(jkey);
    env->DeleteLocalRef(jlabel);
    env->DeleteLocalRef(cls);
}

void show_button_menu(GameActivity* activity, int index, const std::string& label) {
    ScopedEnv scoped(activity != nullptr ? activity->vm : nullptr);
    JNIEnv* env = activity_env(activity, scoped);
    if (env == nullptr) {
        return;
    }

    jclass cls = env->GetObjectClass(activity->javaGameActivity);
    jmethodID method = env->GetMethodID(cls, "showButtonMenu", "(ILjava/lang/String;)V");
    if (method == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        DZ_WARN("buttons: MainActivity has no showButtonMenu()");
        return;
    }

    jstring jlabel = env->NewStringUTF(label.c_str());
    env->CallVoidMethod(activity->javaGameActivity, method, index, jlabel);
    report_exception(env);
    env->DeleteLocalRef(jlabel);
    env->DeleteLocalRef(cls);
}

void drain_button_events(std::vector<ButtonEdit>& edits, std::vector<ButtonCommand>& commands) {
    std::lock_guard lock(g_inbox_mutex);
    edits.swap(g_edits);
    commands.swap(g_commands);
    g_edits.clear();
    g_commands.clear();
}

void drain_preset_events(std::vector<PresetCommand>& commands) {
    std::lock_guard lock(g_inbox_mutex);
    commands.swap(g_preset_commands);
    g_preset_commands.clear();
}

void show_preset_menu(GameActivity* activity, const std::vector<std::string>& names, int current,
                      const std::string& active_window) {
    ScopedEnv scoped(activity != nullptr ? activity->vm : nullptr);
    JNIEnv* env = activity_env(activity, scoped);
    if (env == nullptr) {
        return;
    }

    jclass cls = env->GetObjectClass(activity->javaGameActivity);
    jmethodID method =
        env->GetMethodID(cls, "showPresetMenu", "([Ljava/lang/String;ILjava/lang/String;)V");
    if (method == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        DZ_WARN("presets: MainActivity has no showPresetMenu()");
        return;
    }

    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray array =
        env->NewObjectArray(static_cast<jsize>(names.size()), string_class, nullptr);
    for (std::size_t i = 0; i < names.size(); ++i) {
        jstring item = env->NewStringUTF(names[i].c_str());
        env->SetObjectArrayElement(array, static_cast<jsize>(i), item);
        env->DeleteLocalRef(item);
    }

    jstring window = env->NewStringUTF(active_window.c_str());
    env->CallVoidMethod(activity->javaGameActivity, method, array, current, window);
    report_exception(env);
    env->DeleteLocalRef(window);
    env->DeleteLocalRef(array);
    env->DeleteLocalRef(string_class);
    env->DeleteLocalRef(cls);
}

} // namespace digitiz::guest

// ---------------------------------------------------------------------------
// Called from MainActivity on the UI thread.

extern "C" JNIEXPORT void JNICALL Java_com_onart_digitiz_MainActivity_nativeButtonSaved(
    JNIEnv* env, jclass, jint index, jint kind, jstring label, jint x, jint y, jint w, jint h,
    jint modifiers, jstring key) {
    using namespace digitiz::guest;

    ButtonEdit edit;
    edit.index = index;
    edit.kind = kind;
    edit.label = to_utf8(env, label);
    edit.x = x;
    edit.y = y;
    edit.w = w;
    edit.h = h;
    edit.modifiers = modifiers;
    edit.key = to_utf8(env, key);

    std::lock_guard lock(g_inbox_mutex);
    g_edits.push_back(std::move(edit));
}

extern "C" JNIEXPORT void JNICALL Java_com_onart_digitiz_MainActivity_nativeButtonCommand(
    JNIEnv*, jclass, jint index, jint command) {
    using namespace digitiz::guest;

    std::lock_guard lock(g_inbox_mutex);
    g_commands.push_back(ButtonCommand{index, static_cast<ButtonCommandKind>(command)});
}

extern "C" JNIEXPORT void JNICALL Java_com_onart_digitiz_MainActivity_nativePresetCommand(
    JNIEnv* env, jclass, jint command, jint index, jstring text) {
    using namespace digitiz::guest;

    PresetCommand out;
    out.kind = static_cast<PresetCommandKind>(command);
    out.index = index;
    out.text = to_utf8(env, text);

    std::lock_guard lock(g_inbox_mutex);
    g_preset_commands.push_back(std::move(out));
}
