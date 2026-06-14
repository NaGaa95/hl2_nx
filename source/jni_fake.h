/* jni_fake.h -- minimal fake JNIEnv for the ValveActivity2 JNI exports
 *
 * Unlike CTW, HL2's Java<->native surface is tiny: setArgs/setenv only call
 * env->GetStringUTFChars (vtable slot 169) on jstrings we hand them, so a
 * fake env where jstring == const char* is enough.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

extern void *fake_env; // JNIEnv *

void jni_init(void);

#endif
