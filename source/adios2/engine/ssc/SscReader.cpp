/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * SscReader.cpp
 *
 *  Created on: Nov 1, 2018
 *      Author: Jason Wang
 */

#include "SscReader.tcc"
#include "adios2/helper/adiosComm.h"
#include "adios2/helper/adiosCommMPI.h"
#include "adios2/helper/adiosMpiHandshake.h"

namespace adios2
{
namespace core
{
namespace engine
{

SscReader::SscReader(IO &io, const std::string &name, const Mode mode,
                     helper::Comm comm)
: Engine("SscReader", io, name, mode, std::move(comm))
{
    PERFSTUBS_SCOPED_TIMER_FUNC();

    helper::GetParameter(m_IO.m_Parameters, "Verbose", m_Verbosity);
    helper::GetParameter(m_IO.m_Parameters, "Threading", m_Threading);
    helper::GetParameter(m_IO.m_Parameters, "OpenTimeoutSecs",
                         m_OpenTimeoutSecs);

    helper::Log("Engine", "SSCReader", "Open", m_Name, 0, m_Comm.Rank(), 5,
                m_Verbosity, helper::LogMode::INFO);

    SyncMpiPattern();
}

SscReader::~SscReader() { PERFSTUBS_SCOPED_TIMER_FUNC(); }

void SscReader::BeginStepConsequentFixed()
{
    MPI_Waitall(static_cast<int>(m_MpiRequests.size()), m_MpiRequests.data(),
                MPI_STATUS_IGNORE);
    m_MpiRequests.clear();
}

void SscReader::BeginStepFlexible(StepStatus &status)
{
    m_AllReceivingWriterRanks.clear();
    m_Buffer.resize(1);
    m_Buffer[0] = 0;
    m_GlobalWritePattern.clear();
    m_GlobalWritePattern.resize(m_StreamSize);
    m_LocalReadPattern.clear();
    m_GlobalWritePatternBuffer.clear();
    bool finalStep = SyncWritePattern();
    if (finalStep)
    {
        status = StepStatus::EndOfStream;
        return;
    }
    MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, m_StreamComm, &m_MpiWin);
}

StepStatus SscReader::BeginStep(const StepMode stepMode,
                                const float timeoutSeconds)
{
    PERFSTUBS_SCOPED_TIMER_FUNC();

    ++m_CurrentStep;

    helper::Log("Engine", "SSCReader", "BeginStep",
                std::to_string(CurrentStep()), 0, m_Comm.Rank(), 5, m_Verbosity,
                helper::LogMode::INFO);

    m_StepBegun = true;

    if (m_CurrentStep == 0 || m_WriterDefinitionsLocked == false ||
        m_ReaderSelectionsLocked == false)
    {
        if (m_Threading && m_EndStepThread.joinable())
        {
            m_EndStepThread.join();
        }
        else
        {
            BeginStepFlexible(m_StepStatus);
        }
        if (m_StepStatus == StepStatus::EndOfStream)
        {
            return StepStatus::EndOfStream;
        }
    }
    else
    {
        BeginStepConsequentFixed();
    }

    for (const auto &r : m_GlobalWritePattern)
    {
        for (auto &v : r)
        {
            if (v.shapeId == ShapeID::GlobalValue ||
                v.shapeId == ShapeID::LocalValue)
            {
                std::vector<char> value(v.bufferCount);
                if (m_CurrentStep == 0 || m_WriterDefinitionsLocked == false ||
                    m_ReaderSelectionsLocked == false)
                {
                    std::memcpy(value.data(), v.value.data(), v.value.size());
                }
                else
                {
                    std::memcpy(value.data(), m_Buffer.data() + v.bufferStart,
                                v.bufferCount);
                }

                if (v.type == DataType::String)
                {
                    auto variable = m_IO.InquireVariable<std::string>(v.name);
                    if (variable)
                    {
                        variable->m_Value =
                            std::string(value.begin(), value.end());
                        variable->m_Min =
                            std::string(value.begin(), value.end());
                        variable->m_Max =
                            std::string(value.begin(), value.end());
                    }
                }
#define declare_type(T)                                                        \
    else if (v.type == helper::GetDataType<T>())                               \
    {                                                                          \
        auto variable = m_IO.InquireVariable<T>(v.name);                       \
        if (variable)                                                          \
        {                                                                      \
            std::memcpy(reinterpret_cast<char *>(&variable->m_Min),            \
                        value.data(), value.size());                           \
            std::memcpy(reinterpret_cast<char *>(&variable->m_Max),            \
                        value.data(), value.size());                           \
            std::memcpy(reinterpret_cast<char *>(&variable->m_Value),          \
                        value.data(), value.size());                           \
        }                                                                      \
    }
                ADIOS2_FOREACH_STDTYPE_1ARG(declare_type)
#undef declare_type
                else
                {
                    helper::Log("Engine", "SSCReader", "BeginStep",
                                "unknown data type", 0, m_Comm.Rank(), 0,
                                m_Verbosity, helper::LogMode::ERROR);
                }
            }
        }
    }

    if (m_Buffer[0] == 1)
    {
        return StepStatus::EndOfStream;
    }

    return StepStatus::OK;
}

void SscReader::PerformGets()
{

    helper::Log("Engine", "SSCReader", "PerformGets", "", 0, m_Comm.Rank(), 5,
                m_Verbosity, helper::LogMode::INFO);

    if (m_CurrentStep == 0 || m_WriterDefinitionsLocked == false ||
        m_ReaderSelectionsLocked == false)
    {
        ssc::Deserialize(m_GlobalWritePatternBuffer, m_GlobalWritePattern, m_IO,
                         false, false);
        size_t oldSize = m_AllReceivingWriterRanks.size();
        m_AllReceivingWriterRanks =
            ssc::CalculateOverlap(m_GlobalWritePattern, m_LocalReadPattern);
        CalculatePosition(m_GlobalWritePattern, m_AllReceivingWriterRanks);
        size_t newSize = m_AllReceivingWriterRanks.size();
        if (oldSize != newSize)
        {
            size_t totalDataSize = 0;
            for (auto i : m_AllReceivingWriterRanks)
            {
                totalDataSize += i.second.second;
            }
            m_Buffer.resize(totalDataSize);
            for (const auto &i : m_AllReceivingWriterRanks)
            {
                MPI_Win_lock(MPI_LOCK_SHARED, i.first, 0, m_MpiWin);
                MPI_Get(m_Buffer.data() + i.second.first,
                        static_cast<int>(i.second.second), MPI_CHAR, i.first, 0,
                        static_cast<int>(i.second.second), MPI_CHAR, m_MpiWin);
                MPI_Win_unlock(i.first, m_MpiWin);
            }
        }

        for (auto &br : m_LocalReadPattern)
        {
            if (br.performed)
            {
                continue;
            }
            for (const auto &i : m_AllReceivingWriterRanks)
            {
                const auto &v = m_GlobalWritePattern[i.first];
                for (const auto &b : v)
                {
                    if (b.name == br.name)
                    {
                        if (b.type == DataType::String)
                        {
                            *reinterpret_cast<std::string *>(br.data) =
                                std::string(b.value.begin(), b.value.end());
                        }
#define declare_type(T)                                                        \
    else if (b.type == helper::GetDataType<T>())                               \
    {                                                                          \
        if (b.shapeId == ShapeID::GlobalArray ||                               \
            b.shapeId == ShapeID::LocalArray)                                  \
        {                                                                      \
            bool empty = false;                                                \
            for (const auto c : b.count)                                       \
            {                                                                  \
                if (c == 0)                                                    \
                {                                                              \
                    empty = true;                                              \
                }                                                              \
            }                                                                  \
            if (empty)                                                         \
            {                                                                  \
                continue;                                                      \
            }                                                                  \
            helper::NdCopy<T>(m_Buffer.data<char>() + b.bufferStart, b.start,  \
                              b.count, true, true,                             \
                              reinterpret_cast<char *>(br.data), br.start,     \
                              br.count, true, true);                           \
        }                                                                      \
        else if (b.shapeId == ShapeID::GlobalValue ||                          \
                 b.shapeId == ShapeID::LocalValue)                             \
        {                                                                      \
            std::memcpy(br.data, m_Buffer.data() + b.bufferStart,              \
                        b.bufferCount);                                        \
        }                                                                      \
    }
                        ADIOS2_FOREACH_STDTYPE_1ARG(declare_type)
#undef declare_type
                        else
                        {
                            helper::Log("Engine", "SSCReader", "PerformGets",
                                        "unknown data type", 0, m_Comm.Rank(),
                                        0, m_Verbosity, helper::LogMode::ERROR);
                        }
                    }
                }
            }
            br.performed = true;
        }
    }
}

size_t SscReader::CurrentStep() const { return m_CurrentStep; }

void SscReader::EndStepFixed()
{
    if (m_CurrentStep == 0)
    {
        MPI_Win_free(&m_MpiWin);
        SyncReadPattern();
    }
    for (const auto &i : m_AllReceivingWriterRanks)
    {
        m_MpiRequests.emplace_back();
        MPI_Irecv(m_Buffer.data() + i.second.first,
                  static_cast<int>(i.second.second), MPI_CHAR, i.first, 0,
                  m_StreamComm, &m_MpiRequests.back());
    }
}

void SscReader::EndStepFirstFlexible()
{
    MPI_Win_free(&m_MpiWin);
    SyncReadPattern();
    BeginStepFlexible(m_StepStatus);
}

void SscReader::EndStepConsequentFlexible()
{
    MPI_Win_free(&m_MpiWin);
    BeginStepFlexible(m_StepStatus);
}

void SscReader::EndStep()
{
    PERFSTUBS_SCOPED_TIMER_FUNC();
    helper::Log("Engine", "SSCReader", "EndStep", std::to_string(CurrentStep()),
                0, m_Comm.Rank(), 5, m_Verbosity, helper::LogMode::INFO);

    PerformGets();

    if (m_WriterDefinitionsLocked && m_ReaderSelectionsLocked)
    {
        EndStepFixed();
    }
    else
    {
        if (m_CurrentStep == 0)
        {
            if (m_Threading)
            {
                m_EndStepThread =
                    std::thread(&SscReader::EndStepFirstFlexible, this);
            }
            else
            {
                MPI_Win_free(&m_MpiWin);
                SyncReadPattern();
            }
        }
        else
        {
            if (m_Threading)
            {
                m_EndStepThread =
                    std::thread(&SscReader::EndStepConsequentFlexible, this);
            }
            else
            {
                MPI_Win_free(&m_MpiWin);
            }
        }
    }

    m_StepBegun = false;
}

void SscReader::SyncMpiPattern()
{
    PERFSTUBS_SCOPED_TIMER_FUNC();

    MPI_Group streamGroup;
    MPI_Group readerGroup;
    MPI_Comm writerComm;

    helper::HandshakeComm(m_Name, 'r', m_OpenTimeoutSecs, CommAsMPI(m_Comm),
                          streamGroup, m_WriterGroup, readerGroup, m_StreamComm,
                          writerComm, m_ReaderComm);

    m_ReaderRank = m_Comm.Rank();
    m_ReaderSize = m_Comm.Size();
    MPI_Comm_rank(m_StreamComm, &m_StreamRank);
    MPI_Comm_size(m_StreamComm, &m_StreamSize);

    int writerMasterStreamRank = -1;
    MPI_Allreduce(&writerMasterStreamRank, &m_WriterMasterStreamRank, 1,
                  MPI_INT, MPI_MAX, m_StreamComm);

    int readerMasterStreamRank = -1;
    if (m_ReaderRank == 0)
    {
        readerMasterStreamRank = m_StreamRank;
    }
    MPI_Allreduce(&readerMasterStreamRank, &m_ReaderMasterStreamRank, 1,
                  MPI_INT, MPI_MAX, m_StreamComm);
}

bool SscReader::SyncWritePattern()
{
    PERFSTUBS_SCOPED_TIMER_FUNC();

    ssc::BroadcastMetadata(m_GlobalWritePatternBuffer, m_WriterMasterStreamRank,
                           m_StreamComm);

    if (m_GlobalWritePatternBuffer[0] == 1)
    {
        return true;
    }

    m_WriterDefinitionsLocked = m_GlobalWritePatternBuffer[1];

    ssc::Deserialize(m_GlobalWritePatternBuffer, m_GlobalWritePattern, m_IO,
                     true, true);

    if (m_Verbosity >= 20 && m_ReaderRank == 0)
    {
        ssc::PrintBlockVecVec(m_GlobalWritePattern, "Global Write Pattern");
    }
    return false;
}

void SscReader::SyncReadPattern()
{
    PERFSTUBS_SCOPED_TIMER_FUNC();

    ssc::Buffer localBuffer(8);
    localBuffer.value<uint64_t>() = 8;

    ssc::SerializeVariables(m_LocalReadPattern, localBuffer, m_StreamRank);

    ssc::Buffer globalBuffer;

    ssc::AggregateMetadata(localBuffer, globalBuffer, m_ReaderComm, false,
                           m_ReaderSelectionsLocked);

    ssc::BroadcastMetadata(globalBuffer, m_ReaderMasterStreamRank,
                           m_StreamComm);

    ssc::Deserialize(m_GlobalWritePatternBuffer, m_GlobalWritePattern, m_IO,
                     true, true);

    m_AllReceivingWriterRanks =
        ssc::CalculateOverlap(m_GlobalWritePattern, m_LocalReadPattern);
    CalculatePosition(m_GlobalWritePattern, m_AllReceivingWriterRanks);

    size_t totalDataSize = 0;
    for (auto i : m_AllReceivingWriterRanks)
    {
        totalDataSize += i.second.second;
    }
    m_Buffer.resize(totalDataSize);

    if (m_Verbosity >= 20)
    {
        for (int i = 0; i < m_ReaderSize; ++i)
        {
            m_Comm.Barrier();
            if (i == m_ReaderRank)
            {
                ssc::PrintBlockVec(m_LocalReadPattern,
                                   "\n\nGlobal Read Pattern on Rank " +
                                       std::to_string(m_ReaderRank));
            }
        }
        m_Comm.Barrier();
    }
}

void SscReader::CalculatePosition(ssc::BlockVecVec &bvv,
                                  ssc::RankPosMap &allRanks)
{
    PERFSTUBS_SCOPED_TIMER_FUNC();

    size_t bufferPosition = 0;

    for (int rank = 0; rank < static_cast<int>(bvv.size()); ++rank)
    {
        bool hasOverlap = false;
        for (const auto r : allRanks)
        {
            if (r.first == rank)
            {
                hasOverlap = true;
                break;
            }
        }
        if (hasOverlap)
        {
            allRanks[rank].first = bufferPosition;
            auto &bv = bvv[rank];
            for (auto &b : bv)
            {
                b.bufferStart += bufferPosition;
            }
            size_t currentRankTotalSize = ssc::TotalDataSize(bv);
            allRanks[rank].second = currentRankTotalSize + 1;
            bufferPosition += currentRankTotalSize + 1;
        }
    }
}

#define declare_type(T)                                                        \
    void SscReader::DoGetSync(Variable<T> &variable, T *data)                  \
    {                                                                          \
        helper::Log("Engine", "SSCReader", "GetSync", variable.m_Name, 0,      \
                    m_Comm.Rank(), 5, m_Verbosity, helper::LogMode::INFO);     \
        GetDeferredCommon(variable, data);                                     \
        PerformGets();                                                         \
    }                                                                          \
    void SscReader::DoGetDeferred(Variable<T> &variable, T *data)              \
    {                                                                          \
        helper::Log("Engine", "SSCReader", "GetDeferred", variable.m_Name, 0,  \
                    m_Comm.Rank(), 5, m_Verbosity, helper::LogMode::INFO);     \
        GetDeferredCommon(variable, data);                                     \
    }                                                                          \
    std::vector<typename Variable<T>::BPInfo> SscReader::DoBlocksInfo(         \
        const Variable<T> &variable, const size_t step) const                  \
    {                                                                          \
        return BlocksInfoCommon(variable, step);                               \
    }
ADIOS2_FOREACH_STDTYPE_1ARG(declare_type)
#undef declare_type

void SscReader::DoClose(const int transportIndex)
{
    PERFSTUBS_SCOPED_TIMER_FUNC();

    helper::Log("Engine", "SSCReader", "Close", m_Name, 0, m_Comm.Rank(), 5,
                m_Verbosity, helper::LogMode::INFO);

    if (!m_StepBegun)
    {
        BeginStep();
    }
}

} // end namespace engine
} // end namespace core
} // end namespace adios2
