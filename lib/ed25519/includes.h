/* Dragon stub for the vendored ed25519.c's `#include "includes.h"`.
 *
 * Upstream this is OpenSSH-portable's autoconf-generated global header. Dragon
 * doesn't use OpenSSH's build system, and ed25519.c needs nothing from it
 * beyond what our crypto_api.h provides, so this is intentionally empty. Keeping
 * the include line lets ed25519.c stay byte-identical to upstream. */
#ifndef DRAGON_ED25519_INCLUDES_H
#define DRAGON_ED25519_INCLUDES_H
#endif
