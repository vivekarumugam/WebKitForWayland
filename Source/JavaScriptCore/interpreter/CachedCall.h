/*
 * Copyright (C) 2009, 2013, 2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#ifndef CachedCall_h
#define CachedCall_h

#include "CallFrameClosure.h"
#include "ExceptionHelpers.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "Interpreter.h"
#include "ProtoCallFrame.h"
#include "VMEntryScope.h"
#include "VMInlines.h"

namespace JSC {
    class CachedCall {
        WTF_MAKE_NONCOPYABLE(CachedCall); WTF_MAKE_FAST_ALLOCATED;
    public:
        CachedCall(CallFrame* callFrame, JSFunction* function, int argumentCount)
            : m_valid(false)
            , m_interpreter(callFrame->interpreter())
            , m_entryScope(callFrame->vm(), function->scope()->globalObject())
        {
            ASSERT(!function->isHostFunctionNonInline());
            if (UNLIKELY(callFrame->vm().isSafeToRecurseSoft())) {
                m_arguments.resize(argumentCount);
                m_closure = m_interpreter->prepareForRepeatCall(function->jsExecutable(), callFrame, &m_protoCallFrame, function, argumentCount + 1, function->scope(), m_arguments.data());
            } else
                throwStackOverflowError(callFrame);
            m_valid = !callFrame->hadException();
        }
        
        JSValue call()
        { 
            ASSERT(m_valid);
            return m_interpreter->execute(m_closure);
        }
        void setThis(JSValue v) { m_protoCallFrame.setThisValue(v); }
        void setArgument(int n, JSValue v) { m_protoCallFrame.setArgument(n, v); }

    private:
        bool m_valid;
        Interpreter* m_interpreter;
        VMEntryScope m_entryScope;
        ProtoCallFrame m_protoCallFrame;
        Vector<JSValue> m_arguments;
        CallFrameClosure m_closure;
    };
}

#endif
