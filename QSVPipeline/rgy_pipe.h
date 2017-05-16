﻿// -----------------------------------------------------------------------------------------
// QSVEnc by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2011-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// --------------------------------------------------------------------------------------------

#ifndef _QSV_PIPE_H_
#define _QSV_PIPE_H_

#include <cstdint>
#include <cstdio>
#include <vector>
#include "qsv_osdep.h"
#include "qsv_tchar.h"

enum PipeMode {
    PIPE_MODE_DISABLE = 0,
    PIPE_MODE_ENABLE,
    PIPE_MODE_MUXED, //Stderrのモードに使用し、StderrをStdOutに混合する
};

static const int QSV_PIPE_READ_BUF = 2048;

#if defined(_WIN32) || defined(_WIN64)
typedef HANDLE PIPE_HANDLE;
typedef HANDLE PROCESS_HANDLE;
#else
typedef int PIPE_HANDLE;
typedef pid_t PROCESS_HANDLE;
#endif

typedef struct {
    PIPE_HANDLE h_read;
    PIPE_HANDLE h_write;
    PipeMode mode;
    uint32_t bufferSize;
} PipeSet;

typedef struct {
    PipeSet stdIn;
    PipeSet stdOut;
    PipeSet stdErr;
    FILE *f_stdin;
    uint32_t buf_len;
    char read_buf[QSV_PIPE_READ_BUF];
} ProcessPipe;

class CPipeProcess {
public:
    CPipeProcess() : m_phandle(0) { };
    virtual ~CPipeProcess() { };

    virtual void init() = 0;
    virtual int run(const std::vector<const TCHAR *>& args, const TCHAR *exedir, ProcessPipe *pipes, uint32_t priority, bool hidden, bool minimized) = 0;
    virtual void close() = 0;
    virtual bool processAlive() = 0;
protected:
    virtual int startPipes(ProcessPipe *pipes) = 0;
    PROCESS_HANDLE m_phandle;
};

#if defined(_WIN32) || defined(_WIN64)
class CPipeProcessWin : public CPipeProcess {
public:
    CPipeProcessWin();
    virtual ~CPipeProcessWin();

    virtual void init() override;
    virtual int run(const std::vector<const TCHAR *>& args, const TCHAR *exedir, ProcessPipe *pipes, uint32_t priority, bool hidden, bool minimized) override;
    virtual void close() override;
    virtual bool processAlive() override;
    const PROCESS_INFORMATION& getProcessInfo();
protected:
    virtual int startPipes(ProcessPipe *pipes) override;
    PROCESS_INFORMATION m_pi;
};
#else
class CPipeProcessLinux : public CPipeProcess {
public:
    CPipeProcessLinux();
    virtual ~CPipeProcessLinux();

    virtual void init() override;
    virtual int run(const std::vector<const TCHAR *>& args, const TCHAR *exedir, ProcessPipe *pipes, uint32_t priority, bool hidden, bool minimized) override;
    virtual void close() override;
    virtual bool processAlive() override;
protected:
    virtual int startPipes(ProcessPipe *pipes) override;
};
#endif //#if defined(_WIN32) || defined(_WIN64)

#endif //_QSV_PIPE_H_
