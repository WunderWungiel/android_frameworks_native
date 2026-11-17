/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>

namespace android {
namespace gles {

/*
 * Removes `do { ... } while (false);` wrappers (as emitted by SPIRV-Cross for
 * structured selections) when every break that targets the wrapper is in tail
 * position, i.e. nothing but further such breaks can run after it. Then the
 * wrapper and those breaks are redundant and are replaced with spaces, so all
 * offsets in the source stay unchanged.
 *
 * Returns the number of wrappers removed. Sources that do not parse are left
 * untouched.
 */
size_t flattenTrivialDoWhile(std::string& src);

/*
 * Makes every float/int-family member of every uniform block highp: lowp and
 * mediump qualifiers are replaced, members without a qualifier get one. bool
 * and struct-typed members are left alone. Uniform blocks only exist in GLSL
 * ES 3.00+, where highp is always available in all stages.
 *
 * Returns the number of members changed. Sources that do not parse are left
 * untouched.
 */
size_t promoteUniformBlockPrecision(std::string& src);

} // namespace gles
} // namespace android
