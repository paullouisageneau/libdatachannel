/**
 * Copyright (c) 2021 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "sha.hpp"

#if RTC_ENABLE_WEBSOCKET

#if USE_GNUTLS
#include <nettle/sha1.h>
#else
#include <openssl/sha.h>
#endif

namespace rtc::impl {

binary Sha1(const binary &input) {
#if USE_GNUTLS

binary output(SHA1_DIGEST_SIZE);
struct sha1_ctx ctx;
sha1_init(&ctx);
sha1_update(&ctx, input.size(), input.data());
sha1_digest(&ctx, SHA1_DIGEST_SIZE, output.size());
return output;

#else // USE_GNUTLS==0

binary output(SHA_DIGEST_LENGTH);
SHA_CTX ctx;
SHA1_Init(&ctx);
SHA1_Update(&ctx, input.data(), input.size());
SHA1_Final(reinterpret_cast<unsigned char*>(output.data()), &ctx);
return output;

#endif
}

} // namespace rtc::impl

#endif

