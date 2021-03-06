#include "Debugger.h"

namespace GleeBug
{
    void Debugger::exceptionBreakpoint(const EXCEPTION_RECORD & exceptionRecord, const bool firstChance)
    {
        if (!mProcess->systemBreakpoint) //handle system breakpoint
        {
            //set internal state
            mProcess->systemBreakpoint = true;
            mContinueStatus = DBG_CONTINUE;

            //call the attach callback if appropriate
            if(mAttachedToProcess && mProcess->dwProcessId == mMainProcess.dwProcessId)
                cbAttachBreakpoint();

            //call the callback
            cbSystemBreakpoint();
        }
        else
        {
            //check if the breakpoint exists
            auto foundInfo = mProcess->breakpoints.find({ BreakpointType::Software, ptr(exceptionRecord.ExceptionAddress) });
            if (foundInfo == mProcess->breakpoints.end())
                return;
            const auto info = foundInfo->second;

            //set continue status
            mContinueStatus = DBG_CONTINUE;

            //set back the instruction pointer
            mRegisters->Gip = info.address;

            //restore the original breakpoint byte and do an internal step
            mProcess->MemWriteUnsafe(info.address, info.internal.software.oldbytes, info.internal.software.size);
            mThread->StepInternal(std::bind([this, info]()
            {
                //only restore the bytes if the breakpoint still exists
                if (mProcess->breakpoints.find({ BreakpointType::Software, info.address }) != mProcess->breakpoints.end())
                    mProcess->MemWriteUnsafe(info.address, info.internal.software.newbytes, info.internal.software.size);
            }));

            //call the generic callback
            cbBreakpoint(info);

            //call the user callback
            auto foundCallback = mProcess->breakpointCallbacks.find({ BreakpointType::Software, info.address });
            if (foundCallback != mProcess->breakpointCallbacks.end())
                foundCallback->second(info);

            //delete the breakpoint if it is singleshoot
            if (info.singleshoot)
                mProcess->DeleteGenericBreakpoint(info);
        }
    }

    void Debugger::exceptionSingleStep(const EXCEPTION_RECORD & exceptionRecord, const bool firstChance)
    {
        if (mThread->isInternalStepping) //handle internal steps
        {
            //set internal status
            mThread->isInternalStepping = false;
            mContinueStatus = DBG_CONTINUE;

            //call the internal step callback
            mThread->cbInternalStep();
        }
        if (mThread->isSingleStepping) //handle single step
        {
            //set internal status
            mThread->isSingleStepping = false;
            mContinueStatus = DBG_CONTINUE;

            //call the generic callback
            cbStep();

            //call the user callbacks
            auto cbStepCopy = mThread->stepCallbacks;
            mThread->stepCallbacks.clear();
            for (auto cbStep : cbStepCopy)
                cbStep();
        }
        else //handle hardware breakpoint single step exceptions
        {
            exceptionHardwareBreakpoint(ptr(exceptionRecord.ExceptionAddress));
        }
    }

    void Debugger::exceptionHardwareBreakpoint(ptr exceptionAddress)
    {
        //determine the hardware breakpoint triggered
        ptr dr6 = mRegisters->Dr6();
        HardwareSlot breakpointSlot;
        ptr breakpointAddress;
        if (exceptionAddress == mRegisters->Dr0() || dr6 & 0x1)
        {
            breakpointAddress = mRegisters->Dr0();
            breakpointSlot = HardwareSlot::Dr0;
        }
        else if (exceptionAddress == mRegisters->Dr1() || dr6 & 0x2)
        {
            breakpointAddress = mRegisters->Dr1();
            breakpointSlot = HardwareSlot::Dr1;
        }
        else if (exceptionAddress == mRegisters->Dr2() || dr6 & 0x4)
        {
            breakpointAddress = mRegisters->Dr2();
            breakpointSlot = HardwareSlot::Dr2;
        }
        else if (exceptionAddress == mRegisters->Dr3() || dr6 & 0x8)
        {
            breakpointAddress = mRegisters->Dr3();
            breakpointSlot = HardwareSlot::Dr3;
        }
        else
            return; //not a hardware breakpoint

        //find the breakpoint in the internal structures
        auto foundInfo = mProcess->breakpoints.find({ BreakpointType::Hardware, breakpointAddress });
        if (foundInfo == mProcess->breakpoints.end())
            return; //not a valid hardware breakpoint
        const auto info = foundInfo->second;
        if (info.internal.hardware.slot != breakpointSlot)
            return; //not a valid hardware breakpoint

        //set continue status
        mContinueStatus = DBG_CONTINUE;

        //delete the hardware breakpoint from the thread (not the breakpoint buffer) and do an internal step (TODO: maybe delete from all threads?)
        mThread->DeleteHardwareBreakpoint(breakpointSlot);
        mThread->StepInternal(std::bind([this, info]()
        {
            //only restore if the breakpoint still exists
            if (mProcess->breakpoints.find({ BreakpointType::Hardware, info.address }) != mProcess->breakpoints.end())
                mThread->SetHardwareBreakpoint(info.address, info.internal.hardware.slot, info.internal.hardware.type, info.internal.hardware.size);
        }));

        //call the generic callback
        cbBreakpoint(info);

        //call the user callback
        auto foundCallback = mProcess->breakpointCallbacks.find({ BreakpointType::Hardware, info.address });
        if (foundCallback != mProcess->breakpointCallbacks.end())
            foundCallback->second(info);

        //delete the breakpoint if it is singleshoot
        if (info.singleshoot)
            mProcess->DeleteGenericBreakpoint(info);
    }

    void Debugger::exceptionEvent(const EXCEPTION_DEBUG_INFO & exceptionInfo)
    {
        //let the debuggee handle exceptions per default
        mContinueStatus = DBG_EXCEPTION_NOT_HANDLED;

        const EXCEPTION_RECORD & exceptionRecord = exceptionInfo.ExceptionRecord;
        bool firstChance = exceptionInfo.dwFirstChance == 1;

        //call the debug event callback
        cbExceptionEvent(exceptionInfo);

        //dispatch the exception
        switch (exceptionInfo.ExceptionRecord.ExceptionCode)
        {
        case STATUS_BREAKPOINT:
            exceptionBreakpoint(exceptionRecord, firstChance);
            break;
        case STATUS_SINGLE_STEP:
            exceptionSingleStep(exceptionRecord, firstChance);
            break;
        }

        //call the unhandled exception callback
        if (mContinueStatus == DBG_EXCEPTION_NOT_HANDLED)
            cbUnhandledException(exceptionRecord, firstChance);
    }
};