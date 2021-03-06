#include "processingunit.h"

#define THREADS_PER_LANE 32
#define DEBUG_BUFFER_SIZE 4

namespace argon2 {
namespace opencl {

ProcessingUnit::ProcessingUnit(
        const ProgramContext *programContext, const Argon2Params *params,
        const Device *device, std::size_t batchSize)
    : programContext(programContext), params(params),
      device(device), batchSize(batchSize)
{
    // FIXME: check memSize out of bounds
    auto &clContext = programContext->getContext();
    auto lanes = params->getLanes();
    auto localMemSize = (std::size_t)lanes * ARGON2_BLOCK_SIZE;
    if (programContext->getArgon2Type() == ARGON2_I) {
        localMemSize *= 3;
    } else {
        localMemSize *= 2;
    }

    cmdQueue = cl::CommandQueue(clContext, device->getCLDevice());

    memorySize = params->getMemorySize() * batchSize;
    memoryBuffer = cl::Buffer(clContext, CL_MEM_READ_WRITE, memorySize);
    debugBuffer = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, DEBUG_BUFFER_SIZE);

    mappedMemoryBuffer = cmdQueue.enqueueMapBuffer(
                memoryBuffer, true, CL_MAP_WRITE, 0, memorySize);

    kernel = cl::Kernel(programContext->getProgram(), "argon2_kernel");
    kernel.setArg<cl::Buffer>(0, memoryBuffer);
    kernel.setArg<cl::LocalSpaceArg>(1, { localMemSize });
    kernel.setArg<cl_uint>(2, params->getTimeCost());
    kernel.setArg<cl_uint>(3, lanes);
    kernel.setArg<cl_uint>(4, params->getSegmentBlocks());
}

ProcessingUnit::PasswordWriter::PasswordWriter(
        ProcessingUnit &parent, std::size_t index)
    : params(parent.params),
      type(parent.programContext->getArgon2Type()),
      version(parent.programContext->getArgon2Version()),
      dest(static_cast<std::uint8_t *>(parent.mappedMemoryBuffer))
{
    dest += index * params->getMemorySize();
}

void ProcessingUnit::PasswordWriter::moveForward(std::size_t offset)
{
    dest += offset * params->getMemorySize();
}

void ProcessingUnit::PasswordWriter::moveBackwards(std::size_t offset)
{
    dest -= offset * params->getMemorySize();
}

void ProcessingUnit::PasswordWriter::setPassword(
        const void *pw, std::size_t pwSize) const
{
    params->fillFirstBlocks(dest, pw, pwSize, type, version);
}

ProcessingUnit::HashReader::HashReader(
        ProcessingUnit &parent, std::size_t index)
    : params(parent.params),
      src(static_cast<const std::uint8_t *>(parent.mappedMemoryBuffer)),
      buffer(new std::uint8_t[params->getOutputLength()])
{
    src += index * params->getMemorySize();
}

void ProcessingUnit::HashReader::moveForward(std::size_t offset)
{
    src += offset * params->getMemorySize();
}

void ProcessingUnit::HashReader::moveBackwards(std::size_t offset)
{
    src -= offset * params->getMemorySize();
}

const void *ProcessingUnit::HashReader::getHash() const
{
    params->finalize(buffer.get(), src);
    return buffer.get();
}

void ProcessingUnit::beginProcessing()
{
    cmdQueue.enqueueUnmapMemObject(memoryBuffer, mappedMemoryBuffer);

    cmdQueue.enqueueNDRangeKernel(
                kernel, cl::NullRange,
                cl::NDRange(batchSize, params->getLanes(), THREADS_PER_LANE),
                cl::NDRange(1, params->getLanes(), THREADS_PER_LANE));

    mappedMemoryBuffer = cmdQueue.enqueueMapBuffer(
                memoryBuffer, false, CL_MAP_READ | CL_MAP_WRITE,
                0, memorySize, nullptr, &event);
}

void ProcessingUnit::endProcessing()
{
    event.wait();
    event = cl::Event();
}

} // namespace opencl
} // namespace argon2
