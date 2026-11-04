/*
* Copyright (c) 2026, ethereal contributors. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

// Timeline fences and IDevice::submit on Vulkan (ADR 0015): an IFence is a timeline VkSemaphore, and a submission
// is one vkQueueSubmit with the fence waits and signals in its VkTimelineSemaphoreSubmitInfo.

#include "vulkan-backend.h"

#include <nvrhi/common/misc.h>
#include <sstream>

namespace nvrhi::vulkan
{
    Fence::~Fence()
    {
        if (semaphore)
            m_Context.device.destroySemaphore(semaphore, m_Context.allocationCallbacks);
    }

    uint64_t Fence::getCompletedValue() noexcept
    {
        try
        {
            return m_Context.device.getSemaphoreCounterValue(semaphore);
        }
        catch (const vk::SystemError&)
        {
            return 0;
        }
    }

    FRESULT Fence::wait(uint64_t value, uint64_t timeoutNs) noexcept
    {
        try
        {
            if (m_Context.device.getSemaphoreCounterValue(semaphore) >= value)
                return FS_OK;

            if (timeoutNs == 0)
                return FE_WAIT_TIMEOUT;

            auto waitInfo = vk::SemaphoreWaitInfo()
                .setSemaphores(semaphore)
                .setValues(value);

            // vkWaitSemaphores takes the same nanoseconds; UINT64_MAX (c_InfiniteTimeout) waits without a limit.
            const vk::Result result = m_Context.device.waitSemaphores(waitInfo, timeoutNs);
            return result == vk::Result::eSuccess ? FS_OK : FE_WAIT_TIMEOUT;
        }
        catch (const vk::SystemError& e)
        {
            m_Context.error(std::string("IFence::wait: ") + e.what());
            return FE_GENERIC_ERROR;
        }
    }

    FRESULT Fence::signal(uint64_t value) noexcept
    {
        try
        {
            if (value <= m_Context.device.getSemaphoreCounterValue(semaphore))
            {
                m_Context.error("IFence::signal: the value must increase");
                return FE_INVALID_ARGS;
            }

            m_Context.device.signalSemaphore(vk::SemaphoreSignalInfo().setSemaphore(semaphore).setValue(value));
            return FS_OK;
        }
        catch (const vk::SystemError& e)
        {
            m_Context.error(std::string("IFence::signal: ") + e.what());
            return FE_GENERIC_ERROR;
        }
    }

    NativeObject Fence::getNativeObject(ObjectType objectType) noexcept
    {
        if (objectType == ObjectTypes::VK_Semaphore)
            return NativeObject(VkSemaphore(semaphore));
        return nullptr;
    }

    FenceHandle Device::createFence(const FenceDesc& desc)
    {
        AutoPtr<Fence> fence = TakeOver(MAKE_RC_OBJ(Fence, m_Context));
        fence->desc = desc;

        auto typeInfo = vk::SemaphoreTypeCreateInfo()
            .setSemaphoreType(vk::SemaphoreType::eTimeline)
            .setInitialValue(desc.initialValue);

#ifdef _WIN32
        const auto handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueWin32;
#else
        const auto handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd;
#endif
        auto exportInfo = vk::ExportSemaphoreCreateInfo().setHandleTypes(handleType);
        if (desc.shared)
            typeInfo.setPNext(&exportInfo);

        try
        {
            fence->semaphore = m_Context.device.createSemaphore(vk::SemaphoreCreateInfo().setPNext(&typeInfo),
                m_Context.allocationCallbacks);
        }
        catch (const vk::SystemError& e)
        {
            std::stringstream ss;
            ss << "Failed to create fence " << utils::DebugNameToString(desc.debugName) << ": " << e.what();
            m_Context.error(ss.str());
            return nullptr;
        }

        m_Context.nameVKObject(VkSemaphore(fence->semaphore), vk::ObjectType::eSemaphore,
            vk::DebugReportObjectTypeEXT::eSemaphore, desc.debugName.c_str());

        return fence;
    }

    uint64_t Device::submit(const SubmitDesc& desc) noexcept
    {
        Queue* queue = getQueue(desc.queue);
        if (!queue)
        {
            m_Context.error("submit: the queue does not exist on this device");
            return 0;
        }

        const uint64_t submissionID = queue->submit(desc.commandLists, desc.numCommandLists,
            desc.waits, desc.numWaits, desc.signals, desc.numSignals);

        for (uint32_t index = 0; index < desc.numCommandLists; ++index)
            checked_cast<CommandList*>(desc.commandLists[index])->executed(*queue, submissionID);

        return submissionID;
    }

    FRESULT Device::queueWaitFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        Queue* pQueue = getQueue(queue);
        if (!pQueue || !fence)
            return FE_INVALID_ARGS;

        // A submission of its own: unlike queueWaitForSemaphore, the wait does not attach to someone else's work.
        const FenceValue wait{ fence, value };
        pQueue->submit(nullptr, 0, &wait, 1, nullptr, 0);
        return FS_OK;
    }

    FRESULT Device::queueSignalFence(CommandQueue queue, IFence* fence, uint64_t value) noexcept
    {
        Queue* pQueue = getQueue(queue);
        if (!pQueue || !fence)
            return FE_INVALID_ARGS;

        const FenceValue signal{ fence, value };
        pQueue->submit(nullptr, 0, nullptr, 0, &signal, 1);
        return FS_OK;
    }

    FRESULT Device::waitFences(const FenceValue* fences, uint32_t count, bool waitAll, uint64_t timeoutNs) noexcept
    {
        if (count == 0)
            return FS_OK;
        if (!fences)
            return FE_INVALID_ARGS;

        std::vector<vk::Semaphore> semaphores(count);
        std::vector<uint64_t> values(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            if (!fences[index].fence)
                return FE_INVALID_ARGS;
            semaphores[index] = checked_cast<Fence*>(fences[index].fence)->semaphore;
            values[index] = fences[index].value;
        }

        auto waitInfo = vk::SemaphoreWaitInfo()
            .setFlags(waitAll ? vk::SemaphoreWaitFlags() : vk::SemaphoreWaitFlagBits::eAny)
            .setSemaphores(semaphores)
            .setValues(values);

        try
        {
            const vk::Result result = m_Context.device.waitSemaphores(waitInfo, timeoutNs);
            return result == vk::Result::eSuccess ? FS_OK : FE_WAIT_TIMEOUT;
        }
        catch (const vk::SystemError& e)
        {
            m_Context.error(std::string("waitFences: ") + e.what());
            return FE_GENERIC_ERROR;
        }
    }

} // namespace nvrhi::vulkan
