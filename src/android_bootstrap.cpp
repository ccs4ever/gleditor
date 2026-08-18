/**
 * @file android_bootstrap.cpp
 * @brief Implementation of gleditor::androidBootstrap.
 */
#include <gleditor/android_bootstrap.hpp> // IWYU pragma: associated

#ifdef __ANDROID__

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_system.h>
#include <jni.h>

namespace gleditor {

namespace {

// dlopen() only runs a library's constructors after every constructor of the
// shared objects it depends on has already run -- that is what normally
// guarantees glib's own bootstrap constructor (glib_init_ctor, see glib's
// glib-init.c) finishes before any *mm binding's constructors touch glib.
// That guarantee does not hold here: glib, glibmm and pangomm are all
// statically linked into this one libmain.so (see /CMakeLists.txt), so every
// translation unit's constructors, from all three, are merged into a single
// .init_array with no ordering between them -- confirmed by symbolizing the
// crash from a CI build: glibmm's own static `Glib::Class::iface_properties_
// quark` runs *before* glib_init_ctor and calls g_quark_from_string()
// directly, which -- unlike glib's own gobject_init(), which explicitly
// calls glib_init() first for exactly this reason -- does not itself ensure
// glib has bootstrapped. That leaves GLib's quark table (and its
// once-only-guard) touched before g_quark_init() has ever run; when
// glib_init_ctor (or gobject_init_ctor, chained through it) finally does run
// and calls g_quark_init() for real, its g_assert(quark_seq_id == 0) fails,
// aborting before main() is ever reached.
//
// GLib's own bug tracker (bugzilla.gnome.org #756139, the same ctor-order
// problem between glib and gobject that gobject_init()'s explicit
// GLIB_PRIVATE_CALL(glib_init)() was added to fix) settled on exactly this
// pattern for the general case: "if ctor B depends on ctor A already having
// run, ctor B should call ctor A" rather than relying on link order. There
// is no public API for that here -- ordinary GLib calls all assume
// glib_init() already ran, and g_type_init()/g_type_ensure() are pure
// checks that do not perform it -- so this reaches for glib_init() itself:
// exported (used by gobject's own private cross-library call), not declared
// in any public header, but idempotent by its own construction, so calling
// it extra early is harmless even after glib_init_ctor/gobject_init_ctor
// run it again later. Constructor priority 101 -- the lowest number user
// code may use, since 0-100 is reserved for the implementation -- is what
// gets this called before glibmm's own (unprioritized, and therefore
// later-running) constructor.
extern "C" void glib_init();

__attribute__((constructor(101))) void primeGlibBootstrap() { glib_init(); }

// The full set of files the GL/GLES and Vulkan backends open by path, kept in
// step with assets/shaders/*.glsl and the SPIRV list in the Makefile. Small
// and fixed, so copying the lot on every launch is simpler than tracking
// whether it changed -- a few kilobytes of text and SPIR-V, not an asset
// pack.
constexpr std::array<const char *, 8> kShaderAssets = {
    "shaders/glyph.vert.glsl",       "shaders/glyph.frag.glsl",
    "shaders/beam.vert.glsl",        "shaders/beam.frag.glsl",
    "shaders/vulkan/glyph.vert.spv", "shaders/vulkan/glyph.frag.spv",
    "shaders/vulkan/beam.vert.spv",  "shaders/vulkan/beam.frag.spv",
};

/// Copies one asset out of the APK. SDL_LoadFile resolves a relative path
/// through the Android asset manager, which is what makes the source side of
/// this a normal-looking read; the write side is a plain file because
/// GLEDITOR_ASSET_DIR must name somewhere std::ifstream can later open.
void copyAsset(const std::filesystem::path &destRoot, const char *relative) {
  std::size_t size = 0;
  void *data       = SDL_LoadFile(relative, &size);
  if (nullptr == data) {
    // Left for whatever opens it later to report by name, same as a missing
    // asset on any other platform.
    return;
  }
  const auto destPath = destRoot / relative;
  std::error_code err;
  std::filesystem::create_directories(destPath.parent_path(), err);
  std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
  out.write(static_cast<const char *>(data),
            static_cast<std::streamsize>(size));
  SDL_free(data);
}

void extractShaderAssets(const std::filesystem::path &internal) {
  const std::filesystem::path assetRoot = internal / "assets";
  for (const char *asset : kShaderAssets) {
    copyAsset(assetRoot, asset);
  }
  setenv("GLEDITOR_ASSET_DIR", assetRoot.string().c_str(), 1);
}

/// Android has no fontconfig of its own; without one, Pango finds no fonts at
/// all. This writes out the bundled fonts.conf (assets/fontconfig/fonts.conf,
/// which points at /system/fonts and restates the generic family aliases a
/// desktop fontconfig install would otherwise supply) with its cache
/// directory placeholder filled in, and points FONTCONFIG_FILE at the result.
void extractFontConfig(const std::filesystem::path &internal) {
  std::size_t size = 0;
  void *data       = SDL_LoadFile("fontconfig/fonts.conf", &size);
  if (nullptr == data) {
    return;
  }
  std::string conf(static_cast<const char *>(data), size);
  SDL_free(data);

  const auto cacheDir = internal / "fontconfig-cache";
  std::error_code err;
  std::filesystem::create_directories(cacheDir, err);

  const std::string placeholder = "@@CACHE_DIR@@";
  if (const auto pos = conf.find(placeholder); std::string::npos != pos) {
    conf.replace(pos, placeholder.size(), cacheDir.string());
  }

  const auto destPath = internal / "fontconfig" / "fonts.conf";
  std::filesystem::create_directories(destPath.parent_path(), err);
  std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
  out.write(conf.data(), static_cast<std::streamsize>(conf.size()));

  setenv("FONTCONFIG_FILE", destPath.string().c_str(), 1);
}

/// Reads a "backend" extra off the launch intent, when the app was started
/// with one, and applies it the same way GLEDITOR_BACKEND already works
/// everywhere else. SDL's stock Java Activity is used unmodified, so this --
/// JNI calls against the Activity and Intent SDL itself hands back -- is the
/// only way in that does not mean writing a custom one.
void applyBackendOverrideFromIntent() {
  auto *env     = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (nullptr == env || nullptr == activity) {
    return;
  }

  const jclass activityClass = env->GetObjectClass(activity);
  const jmethodID getIntent  = env->GetMethodID(activityClass, "getIntent",
                                                "()Landroid/content/Intent;");
  const jobject intent       = nullptr != getIntent
                                   ? env->CallObjectMethod(activity, getIntent)
                                   : nullptr;
  if (nullptr == intent) {
    return;
  }

  const jclass intentClass       = env->GetObjectClass(intent);
  const jmethodID getStringExtra = env->GetMethodID(
      intentClass, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
  if (nullptr == getStringExtra) {
    return;
  }
  const jstring key = env->NewStringUTF("backend");
  const auto value =
      static_cast<jstring>(env->CallObjectMethod(intent, getStringExtra, key));
  env->DeleteLocalRef(key);
  if (nullptr == value) {
    return;
  }
  const char *chars = env->GetStringUTFChars(value, nullptr);
  setenv("GLEDITOR_BACKEND", chars, 1);
  env->ReleaseStringUTFChars(value, chars);
  env->DeleteLocalRef(value);
}

/// The internal-storage copy openDocumentFromIntent() last made of a
/// content:// Uri, and the Uri itself (as uri.toString(), reparsed with
/// Uri.parse() rather than kept as a JNI global reference, which would need
/// explicit cleanup this process never has an occasion to run). Empty when
/// nothing has been opened from one yet, or the last thing opened was a
/// file:// Uri or no Uri at all. One remembered document rather than a map
/// keyed by path: gleditor opens at most one document from an intent per
/// process (subsequent "new"/"open" commands make documents with nowhere
/// external to write back to), so androidSaveDocument() only ever needs to
/// ask "is this the one".
std::string &rememberedInternalPath() {
  static std::string path;
  return path;
}
std::string &rememberedOriginalUri() {
  static std::string uri;
  return uri;
}

/// Copies @p uri's bytes into internal storage and returns the resulting
/// path, or an empty string on failure. android.content.ContentResolver.
/// openInputStream() is the only way to read a content:// Uri's bytes --
/// there is no filesystem path behind one in general, unlike the file://
/// scheme openDocumentFromIntent() otherwise handles directly. Copying
/// instead of streaming straight into a TextSource keeps FileTextSource
/// (src/text_source.cpp) the one place that reads a document off disk, on
/// every platform.
std::string
copyOpenableUriToInternalStorage(JNIEnv *env, jobject activity, jobject uri,
                                 const std::filesystem::path &internal) {
  const jclass activityClass = env->GetObjectClass(activity);
  const jmethodID getContentResolver =
      env->GetMethodID(activityClass, "getContentResolver",
                       "()Landroid/content/ContentResolver;");
  const jobject resolver =
      nullptr != getContentResolver
          ? env->CallObjectMethod(activity, getContentResolver)
          : nullptr;
  if (nullptr == resolver) {
    return {};
  }

  const jclass resolverClass = env->GetObjectClass(resolver);
  const jmethodID openInputStream =
      env->GetMethodID(resolverClass, "openInputStream",
                       "(Landroid/net/Uri;)Ljava/io/InputStream;");
  if (nullptr == openInputStream) {
    return {};
  }
  const jobject stream = env->CallObjectMethod(resolver, openInputStream, uri);
  // A Uri that no longer resolves (the app that shared it has since been
  // uninstalled, a permission grant expired, ...) throws rather than
  // returning null -- checked and cleared here, since a pending Java
  // exception makes every further JNI call in this process undefined.
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return {};
  }
  if (nullptr == stream) {
    return {};
  }

  const jclass streamClass = env->GetObjectClass(stream);
  const jmethodID read     = env->GetMethodID(streamClass, "read", "([B)I");
  const jmethodID close    = env->GetMethodID(streamClass, "close", "()V");

  std::error_code err;
  const auto destDir = internal / "opened";
  std::filesystem::create_directories(destDir, err);
  // A fixed name rather than one derived from the Uri: FileTextSource
  // (src/text_source.cpp) and the rest of the library treat a document as an
  // opaque byte stream -- gleditor "names no document format" (README.md) --
  // so there is nothing here that reads an extension, and a fixed name means
  // this never has to sanitise whatever a content provider's own display
  // name happens to contain.
  const auto destPath = destDir / "opened-document";
  std::ofstream out(destPath, std::ios::binary | std::ios::trunc);

  constexpr jsize kChunkSize = 1 << 16;
  const jbyteArray chunk     = env->NewByteArray(kChunkSize);
  std::vector<char> buffer(kChunkSize);
  for (;;) {
    const jint got = env->CallIntMethod(stream, read, chunk);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      break;
    }
    if (got < 0) {
      break; // end of stream
    }
    if (got > 0) {
      env->GetByteArrayRegion(chunk, 0, got,
                              reinterpret_cast<jbyte *>(buffer.data()));
      out.write(buffer.data(), got);
    }
  }
  env->DeleteLocalRef(chunk);
  env->CallVoidMethod(stream, close);
  env->DeleteLocalRef(stream);

  return destPath.string();
}

/// Reads whatever document the app was launched to open -- either
/// android.intent.action.VIEW's own data Uri (opening a file from a file
/// manager or browser) or android.intent.action.SEND's EXTRA_STREAM (sharing
/// a file in from another app) -- and points GLEDITOR_OPEN_FILE at it, which
/// apps/gleditor/main.cpp falls back to when no file was named on the (empty,
/// on Android) command line. A file:// Uri already names a real path, so it
/// is used directly; anything else (content://, almost always) is read
/// through ContentResolver, since that is the only access a Uri grant from
/// another app's Intent gives -- there is no filesystem permission that
/// would let this open one by path instead, and none is asked for.
void openDocumentFromIntent(const std::filesystem::path &internal) {
  auto *env     = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (nullptr == env || nullptr == activity) {
    return;
  }

  const jclass activityClass = env->GetObjectClass(activity);
  const jmethodID getIntent  = env->GetMethodID(activityClass, "getIntent",
                                                "()Landroid/content/Intent;");
  const jobject intent       = nullptr != getIntent
                                   ? env->CallObjectMethod(activity, getIntent)
                                   : nullptr;
  if (nullptr == intent) {
    return;
  }
  const jclass intentClass = env->GetObjectClass(intent);

  const jmethodID getAction =
      env->GetMethodID(intentClass, "getAction", "()Ljava/lang/String;");
  const auto actionValue =
      nullptr != getAction
          ? static_cast<jstring>(env->CallObjectMethod(intent, getAction))
          : nullptr;
  if (nullptr == actionValue) {
    return;
  }
  const char *actionChars = env->GetStringUTFChars(actionValue, nullptr);
  const std::string action(actionChars);
  env->ReleaseStringUTFChars(actionValue, actionChars);
  env->DeleteLocalRef(actionValue);

  jobject uri = nullptr;
  if ("android.intent.action.VIEW" == action) {
    const jmethodID getData =
        env->GetMethodID(intentClass, "getData", "()Landroid/net/Uri;");
    if (nullptr != getData) {
      uri = env->CallObjectMethod(intent, getData);
    }
  } else if ("android.intent.action.SEND" == action) {
    // The one-argument overload, deprecated since API 33 in favour of one
    // that also takes a Class<T> -- kept here since it is still present and
    // functional on every API level this app runs on, and the two-argument
    // one is not.
    const jmethodID getParcelableExtra =
        env->GetMethodID(intentClass, "getParcelableExtra",
                         "(Ljava/lang/String;)Landroid/os/Parcelable;");
    if (nullptr != getParcelableExtra) {
      const jstring key = env->NewStringUTF("android.intent.extra.STREAM");
      uri = env->CallObjectMethod(intent, getParcelableExtra, key);
      env->DeleteLocalRef(key);
    }
  }
  if (nullptr == uri) {
    return;
  }

  const jclass uriClass = env->GetObjectClass(uri);
  const jmethodID getScheme =
      env->GetMethodID(uriClass, "getScheme", "()Ljava/lang/String;");
  const auto schemeValue =
      nullptr != getScheme
          ? static_cast<jstring>(env->CallObjectMethod(uri, getScheme))
          : nullptr;
  std::string scheme;
  if (nullptr != schemeValue) {
    const char *schemeChars = env->GetStringUTFChars(schemeValue, nullptr);
    scheme                  = schemeChars;
    env->ReleaseStringUTFChars(schemeValue, schemeChars);
    env->DeleteLocalRef(schemeValue);
  }

  std::string path;
  if ("file" == scheme) {
    const jmethodID getPath =
        env->GetMethodID(uriClass, "getPath", "()Ljava/lang/String;");
    const auto pathValue =
        nullptr != getPath
            ? static_cast<jstring>(env->CallObjectMethod(uri, getPath))
            : nullptr;
    if (nullptr != pathValue) {
      const char *pathChars = env->GetStringUTFChars(pathValue, nullptr);
      path                  = pathChars;
      env->ReleaseStringUTFChars(pathValue, pathChars);
      env->DeleteLocalRef(pathValue);
    }
  } else {
    path = copyOpenableUriToInternalStorage(env, activity, uri, internal);
    if (!path.empty()) {
      const jmethodID toString =
          env->GetMethodID(uriClass, "toString", "()Ljava/lang/String;");
      const auto uriStringValue =
          nullptr != toString
              ? static_cast<jstring>(env->CallObjectMethod(uri, toString))
              : nullptr;
      if (nullptr != uriStringValue) {
        const char *uriChars = env->GetStringUTFChars(uriStringValue, nullptr);
        rememberedInternalPath() = path;
        rememberedOriginalUri()  = uriChars;
        env->ReleaseStringUTFChars(uriStringValue, uriChars);
        env->DeleteLocalRef(uriStringValue);
      }
    }
  }
  env->DeleteLocalRef(uri);

  if (!path.empty()) {
    setenv("GLEDITOR_OPEN_FILE", path.c_str(), 1);
  }
}

} // namespace

void androidBootstrap() {
  const char *internal = SDL_GetAndroidInternalStoragePath();
  if (nullptr != internal && '\0' != internal[0]) {
    extractShaderAssets(internal);
    extractFontConfig(internal);
    openDocumentFromIntent(internal);
  }
  applyBackendOverrideFromIntent();
}

bool androidSaveDocument(const std::string &documentPath,
                         const std::string &content) {
  if (documentPath.empty() || documentPath != rememberedInternalPath() ||
      rememberedOriginalUri().empty()) {
    return false;
  }

  auto *env     = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (nullptr == env || nullptr == activity) {
    return false;
  }

  const jclass uriClass = env->FindClass("android/net/Uri");
  const jmethodID parse =
      nullptr != uriClass
          ? env->GetStaticMethodID(uriClass, "parse",
                                   "(Ljava/lang/String;)Landroid/net/Uri;")
          : nullptr;
  if (nullptr == parse) {
    return false;
  }
  const jstring uriString = env->NewStringUTF(rememberedOriginalUri().c_str());
  const jobject uri = env->CallStaticObjectMethod(uriClass, parse, uriString);
  env->DeleteLocalRef(uriString);
  if (nullptr == uri) {
    return false;
  }

  const jclass activityClass = env->GetObjectClass(activity);
  const jmethodID getContentResolver =
      env->GetMethodID(activityClass, "getContentResolver",
                       "()Landroid/content/ContentResolver;");
  const jobject resolver =
      nullptr != getContentResolver
          ? env->CallObjectMethod(activity, getContentResolver)
          : nullptr;
  if (nullptr == resolver) {
    env->DeleteLocalRef(uri);
    return false;
  }

  const jclass resolverClass       = env->GetObjectClass(resolver);
  const jmethodID openOutputStream = env->GetMethodID(
      resolverClass, "openOutputStream",
      "(Landroid/net/Uri;Ljava/lang/String;)Ljava/io/OutputStream;");
  if (nullptr == openOutputStream) {
    env->DeleteLocalRef(uri);
    return false;
  }
  // "wt": truncate before writing, matching what overwriting a plain file
  // (Glib::file_set_contents, the non-Android save path) already does. The
  // plain "w" ContentResolver itself documents also truncates, but only for
  // providers that interpret it that way, which is not guaranteed of every
  // one a Uri could have come from.
  const jstring mode = env->NewStringUTF("wt");
  const jobject stream =
      env->CallObjectMethod(resolver, openOutputStream, uri, mode);
  env->DeleteLocalRef(mode);
  env->DeleteLocalRef(uri);
  // A Uri whose write grant has since been revoked, or whose provider no
  // longer exists, throws here rather than returning null.
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  if (nullptr == stream) {
    return false;
  }

  const jclass streamClass = env->GetObjectClass(stream);
  const jmethodID write    = env->GetMethodID(streamClass, "write", "([BII)V");
  const jmethodID close    = env->GetMethodID(streamClass, "close", "()V");

  const auto size        = static_cast<jsize>(content.size());
  const jbyteArray bytes = env->NewByteArray(size);
  env->SetByteArrayRegion(bytes, 0, size,
                          reinterpret_cast<const jbyte *>(content.data()));
  env->CallVoidMethod(stream, write, bytes, 0, size);
  const bool writeFailed = env->ExceptionCheck();
  if (writeFailed) {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(bytes);
  env->CallVoidMethod(stream, close);
  env->DeleteLocalRef(stream);

  return !writeFailed;
}

} // namespace gleditor

#endif // __ANDROID__
