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

// Replays a recording on a stream: the CommandExecutor of slang-rhi's CUDA backend, over the records of
// cuda-commandlist.cpp. Every call is stream-ordered; the only host synchronization is an indirect dispatch, which
// reads its group counts first.

#include "cuda-backend.h"

#include <cstring>

namespace nvrhi::cuda
{
    namespace
    {
        bool Launch(const Context& context, CUstream stream, const CommandLaunch& command, const uint32_t* grid)
        {
            if (command.globalsDst && command.globalsSize)
            {
                if (!NVRHI_CUDA_CHECK(context, cuMemcpyAsync(command.globalsDst, command.globalsSrc, command.globalsSize, stream)))
                    return false;
            }

            const void* params = &command + 1;
            size_t paramsSize = command.paramsSize;
            void* extra[] = {
                CU_LAUNCH_PARAM_BUFFER_POINTER, const_cast<void*>(params),
                CU_LAUNCH_PARAM_BUFFER_SIZE, &paramsSize,
                CU_LAUNCH_PARAM_END
            };

            return NVRHI_CUDA_CHECK(context, cuLaunchKernel(command.function, grid[0], grid[1], grid[2],
                command.block[0], command.block[1], command.block[2], 0, stream, nullptr,
                command.paramsSize ? extra : nullptr));
        }
    }

    bool ExecuteCommands(const Context& context, CUstream stream, CommandListInstance& instance)
    {
        const uint8_t* cursor = instance.commands.data();
        const uint8_t* end = cursor + instance.commands.size();

        while (cursor < end)
        {
            const CommandHeader& header = *reinterpret_cast<const CommandHeader*>(cursor);
            bool ok = true;

            switch (header.type)
            {
            case CommandType::CopyMemory:
            {
                const auto& command = reinterpret_cast<const CommandCopyMemory&>(header);
                ok = NVRHI_CUDA_CHECK(context, cuMemcpyAsync(command.dst, command.src, command.size, stream));
                break;
            }
            case CommandType::SetMemory32:
            {
                const auto& command = reinterpret_cast<const CommandSetMemory32&>(header);
                ok = NVRHI_CUDA_CHECK(context, cuMemsetD32Async(command.dst, command.value, command.count, stream));
                break;
            }
            case CommandType::Copy3D:
            {
                const auto& command = reinterpret_cast<const CommandCopy3D&>(header);
                ok = NVRHI_CUDA_CHECK(context, cuMemcpy3DAsync(&command.copy, stream));
                break;
            }
            case CommandType::Launch:
            {
                const auto& command = reinterpret_cast<const CommandLaunch&>(header);
                ok = Launch(context, stream, command, command.grid);
                break;
            }
            case CommandType::LaunchIndirect:
            {
                // The group counts are written by earlier GPU work: wait for the stream and read them, as the CUDA
                // backend of slang-rhi does.
                const auto& command = reinterpret_cast<const CommandLaunch&>(header);
                uint32_t grid[3] = {};
                ok = NVRHI_CUDA_CHECK(context, cuMemcpyDtoHAsync(grid, command.indirectArgs, sizeof(grid), stream)) &&
                    NVRHI_CUDA_CHECK(context, cuStreamSynchronize(stream));
                if (ok && grid[0] && grid[1] && grid[2])
                    ok = Launch(context, stream, command, grid);
                break;
            }
            case CommandType::NativeWork:
            {
                const auto& command = reinterpret_cast<const CommandNativeWork&>(header);
                command.work->execute(NativeObject(stream));
                break;
            }
            case CommandType::RecordEvent:
            {
                const auto& command = reinterpret_cast<const CommandRecordEvent&>(header);
                ok = NVRHI_CUDA_CHECK(context, cuEventRecord(command.event, stream));
                break;
            }
            case CommandType::PatchTraversable:
            {
                // The block that holds the handle is copied to the device by a later command of this stream.
                const auto& command = reinterpret_cast<const CommandPatchTraversable&>(header);
                *command.dst = command.as->traversable;
                if (!command.as->traversable)
                    context.warning("an acceleration structure is bound before it was built");
                break;
            }
            case CommandType::BuildAccelStruct:
            case CommandType::CompactAccelStruct:
            case CommandType::CopyAccelStruct:
            case CommandType::LaunchRays:
                ok = ExecuteRayTracingCommand(context, stream, header, instance);
                break;
            default:
                context.error("unknown command in a recording");
                return false;
            }

            if (!ok)
                return false;
            cursor += header.size;
        }
        return true;
    }
}
