/*
 *      Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _VOLUMES_H_
#define _VOLUMES_H_

#include <glib.h>
#include <luna-service2/lunaservice.h>

/**
 * VolumesInit
 *
 * @brief Register the /volumes category, which answers how full each mounted
 *        volume is and whether it is encrypted.
 */
int VolumesInit(GMainLoop *loop, LSHandle *handle);

#endif
