/*
 * Copyright (c) 2005 Apple Inc. All rights reserved.
 * Copyright (c) 2005-2006 Paul Guyot <pguyot@kallisys.net>,
 * All rights reserved.
 * Copyright (c) 2006-2013 The MacPorts Project
 *
 * @APPLE_BSD_LICENSE_HEADER_START@
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @APPLE_BSD_LICENSE_HEADER_END@
 */

#define DARWINTRACE_USE_PRIVATE_API 1
#include "darwintrace.h"
#include "dtsharedmemory.h"

#include <stdio.h>
#include <unistd.h>

/**
 * Wrapper around \c close(2) to deny closing the file descriptor used by
 * darwintrace to communicate with the control socket. Since we sometimes want
 * to close our socket using \c fclose(3) and that internally calls \c
 * close(2), we need a way to specifically allow closing the socket when we
 * need to. This possibility is the \c __darwintrace_close_sock variable, which
 * will be set to the FD to be closed when closing should be allowed.
 */
static int _dt_close(int fd) {

	FILE *stream = __darwintrace_sock();
	if (stream) {
		int dtsock = fileno(stream);
		if (fd == dtsock && dtsock != __darwintrace_close_sock) {
			errno = EBADF;
			return -1;
		}
	}


    if(fd == __dtsharedmemory_getStatusFileFd() || fd == __dtsharedmemory_getSharedMemoryFileFd())
    {
        //Not resetting fd would causes errors in port autoconf
        if(!__dtsharedmemory_reset_fd())
        {
            errno = EBADF;
            return -1;
        }
    }

	return close(fd);
}

DARWINTRACE_INTERPOSE(_dt_close, close);
