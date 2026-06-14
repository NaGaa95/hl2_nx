/* jni_fake.c -- minimal fake JNIEnv for the ValveActivity2 JNI exports
 *
 * The launcher's setArgs/setenv JNI exports only ever call GetStringUTFChars
 * (slot 169) on the jstrings and never release them. The jstrings we pass in
 * are plain C strings, so GetStringUTFChars just returns the pointer itself.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>

#include "jni_fake.h"
#include "util.h"

#define JNI_TABLE_SIZE 256 // standard JNINativeInterface has 233 entries

static void *jni_functions[JNI_TABLE_SIZE];
static void **jni_env_obj = jni_functions; // JNIEnv = JNINativeInterface**

void *fake_env = &jni_env_obj;

static long jni_unimplemented(void) {
  debugPrintf("JNI: unimplemented function called\n");
  return 0;
}

// slot 169
static const char *GetStringUTFChars_fake(void *env, const char *jstr, unsigned char *is_copy) {
  (void)env;
  if (is_copy)
    *is_copy = 0;
  return jstr;
}

// slot 170
static void ReleaseStringUTFChars_fake(void *env, const char *jstr, const char *utf) {
  (void)env; (void)jstr; (void)utf;
}

// slot 164: GetStringUTFLength
static int GetStringUTFLength_fake(void *env, const char *jstr) {
  (void)env;
  return jstr ? (int)strlen(jstr) : 0;
}

void jni_init(void) {
  for (int i = 0; i < JNI_TABLE_SIZE; i++)
    jni_functions[i] = (void *)jni_unimplemented;
  jni_functions[164] = (void *)GetStringUTFLength_fake;
  jni_functions[169] = (void *)GetStringUTFChars_fake;
  jni_functions[170] = (void *)ReleaseStringUTFChars_fake;
}
