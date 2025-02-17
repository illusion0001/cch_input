/*! @file thread.c

*  @brief 
*
*  @version 1.0.0
*
*  (C) Copyright 2017 GoPro Inc (http://gopro.com/).
*
*  Licensed under either:
*  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0  
*  - MIT license, http://opensource.org/licenses/MIT
*  at your option.
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
*
*/

#include <stdint.h>

#include "thread.h"

#ifdef _WINDOWS
#else

pthread_t GetCurrentThread(void)
{
	return pthread_self();
}

//TODO: How to set the thread affinity on Macintosh and Linux?
void SetThreadAffinityMask(pthread_t thread, uint32_t *thread_affinity_mask)
{
	(void) thread;
	(void) thread_affinity_mask;
}

#endif
