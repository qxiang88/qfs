//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2010/06/11
// Author: Mike Ovsiannikov
//
// Copyright 2010-2012,2016 Quantcast Corporation. All rights reserved.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
//
//----------------------------------------------------------------------------

#include "Writer.h"

#include <sstream>
#include <algorithm>
#include <cerrno>
#include <sstream>
#include <bitset>
#include <string.h>

#include "kfsio/IOBuffer.h"
#include "kfsio/NetManager.h"
#include "kfsio/checksum.h"
#include "kfsio/ITimeout.h"
#include "kfsio/ClientAuthContext.h"
#include "common/kfsdecls.h"
#include "common/MsgLogger.h"
#include "qcdio/QCUtils.h"
#include "qcdio/qcstutils.h"
#include "qcdio/qcdebug.h"
#include "qcdio/QCDLList.h"
#include "RSStriper.h"
#include "KfsOps.h"
#include "utils.h"
#include "KfsClient.h"
#include "Monitor.h"

namespace KFS
{
namespace client
{

using std::min;
using std::max;
using std::string;
using std::ostream;
using std::ostringstream;

// Kfs client write state machine implementation.
class Writer::Impl :
    public QCRefCountedObj,
    private ITimeout,
    private KfsNetClient::OpOwner
{
public:
    typedef QCRefCountedObj::StRef StRef;

    enum
    {
        kErrorNone       = 0,
        kErrorParameters = -EINVAL,
        kErrorTryAgain   = -EAGAIN,
        kErrorFault      = -EFAULT,
        kErrorNoEntry    = -ENOENT,
        kErrorReadOnly   = -EROFS,
        kErrorSeek       = -ESPIPE,
        kErrorIo         = -EIO
    };

    Impl(
        Writer&       inOuter,
        MetaServer&   inMetaServer,
        Completion*   inCompletionPtr,
        int           inMaxRetryCount,
        int           inWriteThreshold,
        int           inMaxPartialBuffersCount,
        int           inTimeSecBetweenRetries,
        int           inOpTimeoutSec,
        int           inIdleTimeoutSec,
        int           inMaxWriteSize,
        const string& inLogPrefix,
        int64_t       inChunkServerInitialSeqNum)
        : QCRefCountedObj(),
          ITimeout(),
          KfsNetClient::OpOwner(),
          mOuter(inOuter),
          mMetaServer(inMetaServer),
          mPathName(),
          mFileId(-1),
          mClosingFlag(false),
          mSleepingFlag(false),
          mErrorCode(0),
          mWriteThreshold(max(0, inWriteThreshold)),
          mPartialBuffersCount(0),
          mPendingCount(0),
          mIdleTimeoutSec(inIdleTimeoutSec),
          mOpTimeoutSec(inOpTimeoutSec),
          mMaxRetryCount(inMaxRetryCount),
          mTimeSecBetweenRetries(inTimeSecBetweenRetries),
          mMaxPartialBuffersCount(inMaxPartialBuffersCount),
          mMaxWriteSize(min((int)CHUNKSIZE,
            (int)((max(0, inMaxWriteSize) + CHECKSUM_BLOCKSIZE - 1) /
                CHECKSUM_BLOCKSIZE * CHECKSUM_BLOCKSIZE))),
          mMaxPendingThreshold(mMaxWriteSize),
          mReplicaCount(-1),
          mRetryCount(0),
          mFileSize(0),
          mOffset(0),
          mOpenChunkBlockSize(CHUNKSIZE),
          mChunkServerInitialSeqNum(inChunkServerInitialSeqNum),
          mCompletionPtr(inCompletionPtr),
          mBuffer(),
          mLogPrefix(inLogPrefix),
          mStats(),
          mNetManager(mMetaServer.GetNetManager()),
          mTruncateOp(0, 0, -1, 0),
          mOpStartTime(0),
          mCompletionDepthCount(0),
          mStriperProcessCount(0),
          mStriperPtr(0)
        { Writers::Init(mWriters); }
    int Open(
        kfsFileId_t inFileId,
        const char* inFileNamePtr,
        Offset      inFileSize,
        int         inStriperType,
        int         inStripeSize,
        int         inStripeCount,
        int         inRecoveryStripeCount,
        int         inReplicaCount)
    {
        if (inFileId <= 0 || ! inFileNamePtr || ! *inFileNamePtr) {
            return kErrorParameters;
        }
        if (0 == inReplicaCount && 0 != inFileSize) {
            // Overwrite and append are not supported with object store files.
            return kErrorSeek;
        }
        if (mFileId > 0) {
            if (inFileId == mFileId &&
                    inFileNamePtr == mPathName) {
                return mErrorCode;
            }
            return kErrorParameters;
        }
        if (IsOpen() && mErrorCode != 0) {
            return (mErrorCode < 0 ? mErrorCode : -mErrorCode);
        }
        if (mClosingFlag || mSleepingFlag) {
            return kErrorTryAgain;
        }
        delete mStriperPtr;
        string theErrMsg;
        mStriperPtr = 0;
        mOpenChunkBlockSize = Offset(CHUNKSIZE);
        mStriperPtr = Striper::Create(
            inStriperType,
            inStripeCount,
            inRecoveryStripeCount,
            inStripeSize,
            inFileSize,
            mLogPrefix,
            *this,
            mOpenChunkBlockSize,
            theErrMsg
        );
        if (! theErrMsg.empty()) {
            KFS_LOG_STREAM_ERROR << mLogPrefix <<
                theErrMsg <<
            KFS_LOG_EOM;
            return kErrorParameters;
        }
        if (! mStriperPtr || mOpenChunkBlockSize < Offset(CHUNKSIZE)) {
            mOpenChunkBlockSize = Offset(CHUNKSIZE);
        }
        mBuffer.Clear();
        mStats.Clear();
        mReplicaCount          = inReplicaCount;
        mFileSize              = inFileSize;
        mPartialBuffersCount   = 0;
        mPathName              = inFileNamePtr;
        mErrorCode             = 0;
        mFileId                = inFileId;
        mTruncateOp.fid        = -1;
        mTruncateOp.pathname   = 0;
        mTruncateOp.fileOffset = mFileSize;
        mRetryCount            = 0;
        mMaxPendingThreshold   = Offset(mMaxWriteSize) *
            (mStriperPtr ? max(1, inStripeCount) : 1);
        return StartWrite();
    }
    int Close()
    {
        if (! IsOpen()) {
            return 0;
        }
        if (mErrorCode != 0) {
            return mErrorCode;
        }
        if (mClosingFlag) {
            return kErrorTryAgain;
        }
        mClosingFlag = true;
        return StartWrite();
    }
    Offset Write(
        IOBuffer& inBuffer,
        Offset    inLength,
        Offset    inOffset,
        bool      inFlushFlag,
        int       inWriteThreshold)
    {
        if (inOffset < 0) {
            return kErrorParameters;
        }
        if (mErrorCode != 0) {
            return (mErrorCode < 0 ? mErrorCode : -mErrorCode);
        }
        if (mClosingFlag || ! IsOpen()) {
            return kErrorParameters;
        }
        if (inLength <= 0) {
            return (
                (ReportCompletion(0, inLength, inOffset) && inFlushFlag) ?
                StartWrite(true) : 0
            );
        }
        if (inOffset != mOffset + mBuffer.BytesConsumable()) {
            if (0 == mReplicaCount) {
                // Non sequential write is not supported with object store
                // files.
                return kErrorSeek;
            }
            // Just flush for now, do not try to optimize buffer rewrite.
            const int thePrevRefCount = GetRefCount();
            const int theRet = Flush();
            if (theRet < 0) {
                return theRet;
            }
            if (thePrevRefCount > GetRefCount()) {
                return (mErrorCode < 0 ? mErrorCode : -mErrorCode);
            }
            mOffset = inOffset;
        }
        if (mMaxPartialBuffersCount == 0 ||
                inLength < IOBufferData::GetDefaultBufferSize() * 2) {
            // If write size is small, then copy it into the last buffer.
            mBuffer.ReplaceKeepBuffersFull(
                &inBuffer, mBuffer.BytesConsumable(), inLength);
        } else {
            if (mBuffer.IsEmpty()) {
                mPartialBuffersCount = 0;
            }
            mBuffer.Move(&inBuffer, inLength);
            mPartialBuffersCount++;
            if (mMaxPartialBuffersCount >= 0 &&
                    mPartialBuffersCount >= mMaxPartialBuffersCount) {
                mBuffer.MakeBuffersFull();
                mPartialBuffersCount = 0;
                mStats.mBufferCompactionCount++;
            }
        }
        if (inWriteThreshold >= 0) {
            mWriteThreshold = inWriteThreshold;
        }
        const int theErrorCode = StartWrite(inFlushFlag);
        return (theErrorCode == 0 ? inLength :
            (theErrorCode < 0 ? theErrorCode : -theErrorCode));
    }
    int Flush()
    {
        const int theErrorCode = StartWrite(true);
        return (theErrorCode < 0 ? theErrorCode : -theErrorCode);
    }
    void Stop()
    {
        while (! Writers::IsEmpty(mWriters)) {
            delete Writers::Front(mWriters);
        }
        if (mTruncateOp.fid >= 0) {
            mMetaServer.Cancel(&mTruncateOp, this);
        }
        if (mSleepingFlag) {
            mNetManager.UnRegisterTimeoutHandler(this);
            mSleepingFlag = false;
        }
        mClosingFlag = false;
        mBuffer.Clear();
    }
    void Shutdown()
    {
        Stop();
        mFileId      = -1;
        mErrorCode   = 0;
    }
    bool IsOpen() const
        { return (mFileId > 0); }
    bool IsClosing() const
        { return (IsOpen() && mClosingFlag); }
    bool IsActive() const
    {
        return (
            IsOpen() && (
                ! mBuffer.IsEmpty() ||
                ! Writers::IsEmpty(mWriters) ||
                mClosingFlag)
        );
    }
    Offset GetPendingSize() const
        { return (GetPendingSizeSelf() + mPendingCount); }
    int SetWriteThreshold(
        int inThreshold)
    {
        const int  theThreshold      = max(0, inThreshold);
        const bool theStartWriteFlag = mWriteThreshold > theThreshold;
        mWriteThreshold = theThreshold;
        return ((theStartWriteFlag && IsOpen() && mErrorCode == 0) ?
            StartWrite() : mErrorCode
        );
    }
    void DisableCompletion()
        { mCompletionPtr = 0; }
    void Register(
        Completion* inCompletionPtr)
    {
        if (inCompletionPtr == mCompletionPtr) {
            return;
        }
        if (mCompletionPtr) {
            mCompletionPtr->Unregistered(mOuter);
        }
        mCompletionPtr = inCompletionPtr;
    }
    bool Unregister(
        Completion* inCompletionPtr)
    {
        if (inCompletionPtr != mCompletionPtr) {
            return false;
        }
        mCompletionPtr = 0;
        return true;
    }
    void GetStats(
        Stats&               outStats,
        KfsNetClient::Stats& outChunkServersStats) const
    {
        outStats             = mStats;
        outChunkServersStats = mChunkServersStats;
    }
    bool GetErrorCode() const
        { return mErrorCode; }

private:
    typedef KfsNetClient ChunkServer;

    class ChunkWriter : public KfsCallbackObj, private KfsNetClient::OpOwner
    {
    public:
        struct WriteOp;
        typedef QCDLList<WriteOp, 0> Queue;
        typedef QCDLList<ChunkWriter, 0> Writers;

        struct WriteOp : public KfsOp
        {
            WritePrepareOp mWritePrepareOp;
            WriteSyncOp    mWriteSyncOp;
            IOBuffer       mBuffer;
            size_t         mBeginBlock;
            size_t         mEndBlock;
            time_t         mOpStartTime;
            bool           mChecksumValidFlag;
            WriteOp*       mPrevPtr[1];
            WriteOp*       mNextPtr[1];

            WriteOp()
                : KfsOp(CMD_WRITE, 0),
                  mWritePrepareOp(0, 0, 0),
                  mWriteSyncOp(),
                  mBuffer(),
                  mBeginBlock(0),
                  mEndBlock(0),
                  mOpStartTime(0),
                  mChecksumValidFlag(false)
                { Queue::Init(*this); }
            void Delete(
                WriteOp** inListPtr)
            {
                Queue::Remove(inListPtr, *this);
                delete this;
            }
            virtual void Request(
                ReqOstream& inStream)
            {
                if (mWritePrepareOp.replyRequestedFlag) {
                    mWritePrepareOp.seq                = seq;
                    mWritePrepareOp.shortRpcFormatFlag = shortRpcFormatFlag;
                } else {
                    mWriteSyncOp.seq                = seq;
                    mWritePrepareOp.seq             = seq + 1;
                    mWriteSyncOp.shortRpcFormatFlag = shortRpcFormatFlag;
                }
                mWritePrepareOp.Request(inStream);
            }
            virtual bool NextRequest(
                kfsSeq_t    inSeqNum,
                ReqOstream& inStream)
            {
                if (mWritePrepareOp.replyRequestedFlag) {
                    return false;
                }
                QCASSERT(seq <= inSeqNum && inSeqNum <= mWritePrepareOp.seq + 1);
                if (mWritePrepareOp.seq < inSeqNum) {
                    return false;
                }
                mWriteSyncOp.Request(inStream);
                return true;
            }
            virtual ostream& ShowSelf(
                ostream& inStream) const
            {
                inStream << mWritePrepareOp.Show();
                if (! mWritePrepareOp.replyRequestedFlag) {
                    inStream << " " << mWriteSyncOp.Show();
                }
                return inStream;
            }
            virtual void ParseResponseHeaderSelf(
                const Properties& inProps)
            {
                if (contentLength > 0) {
                    KFS_LOG_STREAM_ERROR <<
                        "invalid response content length: " << contentLength <<
                        " " << mWriteSyncOp.Show() <<
                    KFS_LOG_EOM;
                    contentLength = 0;
                }
                mWritePrepareOp.status    = status;
                mWritePrepareOp.statusMsg = statusMsg;
                mWriteSyncOp.status       = status;
                mWriteSyncOp.statusMsg    = statusMsg;
                if (mWritePrepareOp.replyRequestedFlag) {
                    mWritePrepareOp.ParseResponseHeaderSelf(inProps);
                } else {
                    mWriteSyncOp.ParseResponseHeaderSelf(inProps);
                }
            }
            void InitBlockRange()
            {
                QCASSERT(
                    mWritePrepareOp.offset >= 0 &&
                    mWritePrepareOp.offset +
                        mBuffer.BytesConsumable() <= (Offset)CHUNKSIZE
                );
                mBeginBlock = mWritePrepareOp.offset / CHECKSUM_BLOCKSIZE;
                mEndBlock   = mBeginBlock +
                    (mBuffer.BytesConsumable() + CHECKSUM_BLOCKSIZE - 1) /
                    CHECKSUM_BLOCKSIZE;
            }
        private:
            virtual ~WriteOp()
                {}
            WriteOp(
                const WriteOp& inWriteOp);
            WriteOp& operator=(
                const WriteOp& inWriteOp);
        };

        ChunkWriter(
            Impl&         inOuter,
            int64_t       inSeqNum,
            const string& inLogPrefix)
            : KfsCallbackObj(),
              KfsNetClient::OpOwner(),
              mOuter(inOuter),
              mChunkServer(
                inOuter.mNetManager,
                string(), -1,
                // All chunk server retries are handled here
                0, // inMaxRetryCount
                0, // inTimeSecBetweenRetries,
                inOuter.mOpTimeoutSec,
                inOuter.mIdleTimeoutSec,
                inSeqNum,
                inLogPrefix.c_str(),
                // Just fail the op. Error handler will reset connection and
                // cancel all pending ops by calling Stop()
                false // inResetConnectionOnOpTimeoutFlag
              ),
              mErrorCode(0),
              mRetryCount(0),
              mPendingCount(0),
              mOpenChunkBlockFileOffset(-1),
              mMaxChunkPos(0),
              mOpStartTime(0),
              mWriteIds(),
              mAllocOp(0, 0, ""),
              mWriteIdAllocOp(0, 0, 0, 0, 0),
              mCloseOp(0, 0),
              mLastOpPtr(0),
              mSleepingFlag(false),
              mClosingFlag(false),
              mLogPrefix(inLogPrefix),
              mOpDoneFlagPtr(0),
              mInFlightBlocks(),
              mHasSubjectIdFlag(false),
              mKeepLeaseFlag(false),
              mLeaseUpdatePendingFlag(false),
              mChunkAccess(),
              mLeaseEndTime(0),
              mLeaseExpireTime(0),
              mChunkAccessExpireTime(0),
              mCSAccessExpireTime(0),
              mUpdateLeaseOp(0, -1, 0),
              mSleepTimer(inOuter.mNetManager, *this)
        {
            SET_HANDLER(this, &ChunkWriter::EventHandler);
            Queue::Init(mPendingQueue);
            Queue::Init(mInFlightQueue);
            Writers::Init(*this);
            Writers::PushFront(mOuter.mWriters, *this);
            mChunkServer.SetRetryConnectOnly(true);
            mAllocOp.fileOffset        = -1;
            mAllocOp.invalidateAllFlag = false;
        }
        ~ChunkWriter()
        {
            ChunkWriter::Shutdown();
            ChunkServer::Stats theStats;
            mChunkServer.GetStats(theStats);
            mOuter.mChunkServersStats.Add(theStats);
            Writers::Remove(mOuter.mWriters, *this);
        }
        void CancelClose()
            { mClosingFlag = false; }
        // The QueueWrite() guarantees that completion will not be invoked.
        // The writes will be queued even if the writer is already in the error
        // state: mErrorCode != 0. In the case of fatal error all pending writes
        // are discarded when the writer gets deleted.
        //
        // StartWrite() must be called in order to start executing pending
        // writes.
        // This allows the caller to properly update its state before the writes
        // get executed, and the corresponding completion(s) invoked.
        Offset QueueWrite(
            IOBuffer& inBuffer,
            Offset    inSize,
            Offset    inOffset,
            int       inWriteThreshold)
        {
            Offset theSize = min(Offset(inBuffer.BytesConsumable()), inSize);
            if (theSize <= 0) {
                return 0;
            }
            const Offset kChunkSize         = (Offset)CHUNKSIZE;
            const int    kChecksumBlockSize = (int)CHECKSUM_BLOCKSIZE;
            QCRTASSERT(inOffset >= 0 && ! mClosingFlag);
            const Offset theChunkOffset = inOffset % kChunkSize;
            if (mAllocOp.fileOffset < 0) {
                mAllocOp.fileOffset = inOffset - theChunkOffset;
                mOpenChunkBlockFileOffset = mAllocOp.fileOffset -
                    mAllocOp.fileOffset % mOuter.mOpenChunkBlockSize;
            } else {
                QCRTASSERT(mAllocOp.fileOffset == inOffset - theChunkOffset);
            }
            theSize = min(theSize, (Offset)(kChunkSize - theChunkOffset));
            mOuter.mStats.mWriteCount++;
            mOuter.mStats.mWriteByteCount += theSize;
            QCASSERT(theSize > 0);
            Offset thePos = theChunkOffset;
            // Try to append to the last pending op.
            WriteOp* const theLastOpPtr = Queue::Back(mPendingQueue);
            if (theLastOpPtr) {
                WriteOp& theOp = *theLastOpPtr;
                const int    theOpSize = theOp.mBuffer.BytesConsumable();
                const Offset theOpPos  = theOp.mWritePrepareOp.offset;
                if (theOpPos + theOpSize == thePos) {
                    const int theHead = (int)(theOpPos % kChecksumBlockSize);
                    int       theNWr  = min(theSize,
                        Offset(theHead == 0 ?
                            mOuter.mMaxWriteSize :
                            kChecksumBlockSize - theHead
                        ) - theOpSize
                    );
                    if (theNWr > 0 &&
                            theOpSize + theNWr > kChecksumBlockSize) {
                        theNWr -= (theOpSize + theNWr) % kChecksumBlockSize;
                    }
                    if (theNWr > 0) {
                        theOp.mBuffer.Move(&inBuffer, theNWr);
                        // Force checksum recomputation.
                        theOp.mChecksumValidFlag = false;
                        theOp.mWritePrepareOp.checksums.clear();
                        // Update last the block index.
                        const int theCurBegin = theOp.mBeginBlock;
                        theOp.InitBlockRange();
                        theOp.mBeginBlock = theCurBegin;
                        // The op is already in the pending queue.
                        theSize -= theNWr;
                        thePos  += theNWr;
                    }
                }
            }
            const int theWriteThreshold = thePos + theSize >= kChunkSize ?
                1 : max(inWriteThreshold, 1);
            const int theBlockOff       = (int)(thePos % kChecksumBlockSize);
            if (theBlockOff > 0 && (theSize >= theWriteThreshold ||
                    theBlockOff + theSize >= kChecksumBlockSize)) {
                WriteOp* const theWriteOpPtr = new WriteOp();
                theWriteOpPtr->mWritePrepareOp.offset = thePos;
                const int theNWr = theWriteOpPtr->mBuffer.Move(
                    &inBuffer,
                    min(theSize, Offset(kChecksumBlockSize - theBlockOff))
                );
                theSize -= theNWr;
                thePos  += theNWr;
                theWriteOpPtr->InitBlockRange();
                Queue::PushBack(mPendingQueue, *theWriteOpPtr);
            }
            while (theSize >= theWriteThreshold) {
                int theOpSize = min(Offset(mOuter.mMaxWriteSize), theSize);
                if (theOpSize > kChecksumBlockSize) {
                    theOpSize -= theOpSize % kChecksumBlockSize;
                }
                WriteOp* const theWriteOpPtr = new WriteOp();
                theWriteOpPtr->mWritePrepareOp.offset = thePos;
                const int theNWr =
                    theWriteOpPtr->mBuffer.Move(&inBuffer, theOpSize);
                theSize -= theNWr;
                thePos  += theNWr;
                theWriteOpPtr->InitBlockRange();
                Queue::PushBack(mPendingQueue, *theWriteOpPtr);
            }
            QCRTASSERT(thePos <= kChunkSize && theSize >= 0);
            const Offset theNWr = thePos - theChunkOffset;
            // The following must be updated before invoking StartWrite(),
            // as it could invoke completion immediately (in the case of
            // failure).
            mPendingCount += theNWr;
            mMaxChunkPos = max(thePos, mMaxChunkPos);
            return theNWr;
        }
        void StartWrite()
        {
            if (mSleepingFlag && ! CancelLeaseUpdate()) {
                return;
            }
            mLeaseUpdatePendingFlag = false;
            if (mErrorCode != 0 && ! mAllocOp.invalidateAllFlag) {
                if (mLastOpPtr) {
                    Reset();
                }
                mClosingFlag = false;
                return;
            }
            if (mClosingFlag && ! CanWrite()) {
                if (! Queue::IsEmpty(mInFlightQueue)) {
                    return;
                }
                if (mLastOpPtr == &mCloseOp) {
                    return;
                }
                // Try to close chunk even if chunk server disconnected, to
                // release the write lease.
                if (mAllocOp.chunkId > 0) {
                    // Wait for write id allocation completion with object store
                    // block write.
                    if (&mWriteIdAllocOp != mLastOpPtr ||
                            mCloseOp.chunkId < 0 ||
                            0 <= mCloseOp.chunkVersion) {
                        CloseChunk();
                    }
                    return;
                }
                if (mKeepLeaseFlag) {
                    if (&mAllocOp != mLastOpPtr &&
                            &mWriteIdAllocOp != mLastOpPtr) {
                        // Re-allocate object block to force to create lease.
                        Reset();
                        AllocateChunk();
                    }
                    return;
                }
                mChunkServer.Stop();
                if (mLastOpPtr == &mAllocOp) {
                    mOuter.mMetaServer.Cancel(mLastOpPtr, this);
                }
                mClosingFlag        = false;
                mAllocOp.fileOffset = -1;
                mAllocOp.chunkId    = -1;
                ReportCompletion();
                return;
            }
            if (! CanWrite() && ! SheduleLeaseUpdate()) {
                return;
            }
            if (0 < mAllocOp.chunkId && min(mLeaseEndTime - 1,
                        mLeaseExpireTime + kLeaseRenewTime / 2) <=
                        Now()) {
                // When chunk server disconnects it might clean up write lease.
                // Start from the beginning -- chunk allocation.
                KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                    "write lease expired: " <<
                        mChunkServer.GetServerLocation() <<
                    " starting from chunk allocation, pending:" <<
                    " queue: " << (Queue::IsEmpty(mPendingQueue) ? "" : "not") <<
                        " empty" <<
                KFS_LOG_EOM;
                Reset();
                if (! CanWrite() && ! SheduleLeaseUpdate()) {
                    // Do not try to preallocate chunk after inactivity timeout
                    // or error, if no data pending.
                    return;
                }
            }
            // Return immediately after calling Write() and AllocateChunk(), as
            // these can invoke completion. Completion, in turn, can delete
            // this.
            // Other methods of this class have to return immediately (unwind)
            // after invoking StartWrite().
            if (mAllocOp.chunkId > 0 && ! mWriteIds.empty()) {
                if (CanWrite()) {
                    Write();
                } else {
                    UpdateLease();
                }
            } else if (! mLastOpPtr) { // Close can be in flight.
                Reset();
                AllocateChunk();
            }
        }
        void Close()
        {
            if (! mClosingFlag && IsOpen()) {
                mClosingFlag = true;
                StartWrite();
            }
        }
        void Shutdown()
        {
            Reset();
            QCRTASSERT(Queue::IsEmpty(mInFlightQueue));
            while (! Queue::IsEmpty(mPendingQueue)) {
                Queue::Front(mPendingQueue)->Delete(mPendingQueue);
            }
            mClosingFlag  = false;
            mErrorCode    = 0;
            mPendingCount = 0;
        }
        Offset GetFileOffset() const
        {
            return (mErrorCode == 0 ? mAllocOp.fileOffset : -1);
        }
        bool IsIdle() const
        {
            return (
                Queue::IsEmpty(mPendingQueue) &&
                Queue::IsEmpty(mInFlightQueue) &&
                ! mClosingFlag
            );
        }
        bool IsOpen() const
        {
            return (
                mErrorCode == 0 &&
                mAllocOp.fileOffset >= 0 &&
                ! mClosingFlag
            );
        }
        int GetErrorCode() const
            { return mErrorCode; }
        Offset GetPendingCount() const
            { return mPendingCount; }
        ChunkWriter* GetPrevPtr()
        {
            ChunkWriter& thePrev = ChunkWritersListOp::GetPrev(*this);
            return (&thePrev == this ? 0 : &thePrev);
        }
        Offset GetOpenChunkBlockFileOffset() const
        {
            return (mAllocOp.fileOffset >= 0 ? mOpenChunkBlockFileOffset : -1);
        }

    private:
        typedef std::vector<WriteInfo>                      WriteIds;
        typedef std::bitset<CHUNKSIZE / CHECKSUM_BLOCKSIZE> ChecksumBlocks;
        typedef NetManager::Timer                           Timer;
        enum { kLeaseRenewTime = LEASE_INTERVAL_SECS / 3 };

        Impl&          mOuter;
        ChunkServer    mChunkServer;
        int            mErrorCode;
        int            mRetryCount;
        Offset         mPendingCount;
        Offset         mOpenChunkBlockFileOffset;
        Offset         mMaxChunkPos;
        time_t         mOpStartTime;
        WriteIds       mWriteIds;
        AllocateOp     mAllocOp;
        WriteIdAllocOp mWriteIdAllocOp;
        CloseOp        mCloseOp;
        KfsOp*         mLastOpPtr;
        bool           mSleepingFlag;
        bool           mClosingFlag;
        string const   mLogPrefix;
        bool*          mOpDoneFlagPtr;
        ChecksumBlocks mInFlightBlocks;
        bool           mHasSubjectIdFlag;
        bool           mKeepLeaseFlag;
        bool           mLeaseUpdatePendingFlag;
        string         mChunkAccess;
        time_t         mLeaseEndTime;
        time_t         mLeaseExpireTime;
        time_t         mChunkAccessExpireTime;
        time_t         mCSAccessExpireTime;
        WritePrepareOp mUpdateLeaseOp;
        Timer          mSleepTimer;
        WriteOp*       mPendingQueue[1];
        WriteOp*       mInFlightQueue[1];
        ChunkWriter*   mPrevPtr[1];
        ChunkWriter*   mNextPtr[1];

        friend class QCDLListOp<ChunkWriter, 0>;
        typedef QCDLListOp<ChunkWriter, 0> ChunkWritersListOp;

        void UpdateLeaseExpirationTime()
        {
            mLeaseExpireTime = min(mLeaseEndTime,
                Now() + LEASE_INTERVAL_SECS - kLeaseRenewTime);
        }
        void AllocateChunk()
        {
            QCASSERT(
                mOuter.mFileId > 0 &&
                mAllocOp.fileOffset >= 0 &&
                (! Queue::IsEmpty(mPendingQueue) ||
                    (0 < mCloseOp.chunkId && mCloseOp.chunkVersion < 0) ||
                    mKeepLeaseFlag)
            );
            Reset(mAllocOp);
            if (0 == mOuter.mReplicaCount) {
                if (! mAllocOp.chunkServers.empty()) {
                    mAllocOp.masterServer = mAllocOp.chunkServers.front();
                }
            } else {
                mAllocOp.masterServer.Reset(0, -1);
            }
            mAllocOp.fid                  = mOuter.mFileId;
            mAllocOp.pathname             = mOuter.mPathName;
            mAllocOp.append               = false;
            mAllocOp.chunkId              = -1;
            mAllocOp.chunkVersion         = -1;
            mAllocOp.spaceReservationSize = 0;
            mAllocOp.maxAppendersPerChunk = 0;
            mAllocOp.allowCSClearTextFlag = false;
            mAllocOp.allCSShortRpcFlag    = false;
            mAllocOp.chunkLeaseDuration            = -1;
            mAllocOp.chunkServerAccessValidForTime = 0;
            mAllocOp.chunkServerAccessIssuedTime   = 0;
            mAllocOp.chunkServers.clear();
            mAllocOp.chunkAccess.clear();
            mAllocOp.chunkServerAccessToken.clear();
            mOuter.mStats.mChunkAllocCount++;
            // Use 5x chunk op timeout for "allocation" that can require
            // chunk version change.
            const int theMetaOpTimeout = mOuter.mMetaServer.GetOpTimeoutSec();
            EnqueueMeta(mAllocOp, 0, max(0, max(mOuter.mOpTimeoutSec,
                    5 * theMetaOpTimeout) - theMetaOpTimeout));
        }
        void Done(
            AllocateOp& inOp,
            bool        inCanceledFlag,
            IOBuffer*   inBufferPtr)
        {
            QCASSERT(&mAllocOp == &inOp && ! inBufferPtr);
            if (inCanceledFlag) {
                return;
            }
            if (inOp.status != 0 || (mAllocOp.chunkServers.empty() &&
                    ! mAllocOp.invalidateAllFlag)) {
                mAllocOp.chunkId = 0;
                HandleError(inOp);
                return;
            }
            if (mAllocOp.invalidateAllFlag) {
                // Report all writes completed. Completion does not expect the
                // offset to match the original write offset with striper.
                KFS_LOG_STREAM_INFO << mLogPrefix <<
                    "invalidate done:"
                    " chunk: "   << mAllocOp.chunkId <<
                    " offset: "  << mAllocOp.fileOffset <<
                    " status: "  << inOp.status <<
                    " pending: " << mPendingCount <<
                    " w-empty: " << Queue::IsEmpty(mPendingQueue) <<
                KFS_LOG_EOM;
                const Offset theSize   = mPendingCount;
                const Offset theOffset = theSize > 0 ? mAllocOp.fileOffset : 0;
                mAllocOp.invalidateAllFlag = false;
                Shutdown();
                ReportCompletion(theOffset, theSize);
                return;
            }
            mLeaseEndTime = Now() + (mAllocOp.chunkLeaseDuration < 0 ?
                time_t(10) * 365 * 24 * 3600 :
                (time_t)max(
                    int64_t(1), mAllocOp.chunkLeaseDuration - kLeaseRenewTime));
            UpdateLeaseExpirationTime();
            mKeepLeaseFlag = mAllocOp.chunkVersion < 0;
            AllocateWriteId();
        }
        bool SheduleLeaseUpdate()
        {
            if (! mKeepLeaseFlag) {
                return false;
            }
            const time_t theNow = Now();
            if (theNow < mLeaseExpireTime) {
                mLeaseUpdatePendingFlag = true;
                Sleep(mLeaseExpireTime - theNow);
                return false;
            }
            return true;
        }
        bool CancelLeaseUpdate()
        {
            if (! mLeaseUpdatePendingFlag) {
                return false;
            }
            if (mSleepingFlag) {
                mSleepTimer.RemoveTimeout();
                mSleepingFlag = false;
            }
            mLeaseUpdatePendingFlag = false;
            return true;
        }
        bool CanWrite()
        {
            return (
                ! Queue::IsEmpty(mPendingQueue) ||
                mAllocOp.invalidateAllFlag
            );
        }
        void AllocateWriteId()
        {
            QCASSERT(mAllocOp.chunkId > 0 && ! mAllocOp.chunkServers.empty());
            Reset(mWriteIdAllocOp);
            mWriteIdAllocOp.chunkId                     = mAllocOp.chunkId;
            mWriteIdAllocOp.chunkVersion                = mAllocOp.chunkVersion;
            mWriteIdAllocOp.isForRecordAppend           = false;
            mWriteIdAllocOp.chunkServerLoc              = mAllocOp.chunkServers;
            mWriteIdAllocOp.offset                      = 0;
            mWriteIdAllocOp.numBytes                    = 0;
            mWriteIdAllocOp.writePrepReplySupportedFlag = false;

            const time_t theNow = Now();
            mHasSubjectIdFlag = false;
            mChunkAccess.clear();

            const bool theCSClearTextAllowedFlag =
                mOuter.IsChunkServerClearTextAllowed();
            mChunkServer.SetShutdownSsl(
                mAllocOp.allowCSClearTextFlag &&
                theCSClearTextAllowedFlag
            );
            mChunkServer.SetRpcFormat(mAllocOp.allCSShortRpcFlag ?
                ChunkServer::kRpcFormatShort : ChunkServer::kRpcFormatLong);
            if (mAllocOp.chunkServerAccessToken.empty() ||
                    mAllocOp.chunkAccess.empty()) {
                mChunkServer.SetKey(0, 0, 0, 0);
                mChunkServer.SetAuthContext(0);
                if (! mAllocOp.chunkServerAccessToken.empty()) {
                    mWriteIdAllocOp.status    = -EINVAL;
                    mWriteIdAllocOp.statusMsg = "no chunk access";
                } else if (! mAllocOp.chunkAccess.empty()) {
                    mWriteIdAllocOp.status    = -EINVAL;
                    mWriteIdAllocOp.statusMsg = "no chunk server access";
                } else if (! theCSClearTextAllowedFlag) {
                    mWriteIdAllocOp.status    = -EPERM;
                    mWriteIdAllocOp.statusMsg =
                        "no clear text chunk server access";
                } else {
                    mChunkAccessExpireTime = theNow + 60 * 60 * 24 * 365;
                    mCSAccessExpireTime    = mChunkAccessExpireTime;
                }
            } else {
                mChunkServer.SetKey(
                    mAllocOp.chunkServerAccessToken.data(),
                    mAllocOp.chunkServerAccessToken.size(),
                    mAllocOp.chunkServerAccessKey.GetPtr(),
                    mAllocOp.chunkServerAccessKey.GetSize()
                );
                mChunkAccess           = mAllocOp.chunkAccess;
                mWriteIdAllocOp.access = mChunkAccess;
                // Always ask for chunk access token here, as the chunk access
                // token's lifetime returned by alloc is 5 min.
                // The chunk returns the token with the corresponding key's
                // lifetime as the token subject includes write id.
                mWriteIdAllocOp.createChunkAccessFlag = true;
                mChunkAccessExpireTime = theNow - 60 * 60 * 24;
                mCSAccessExpireTime = GetAccessExpireTime(
                    theNow,
                    mAllocOp.chunkServerAccessIssuedTime,
                    mAllocOp.chunkServerAccessValidForTime
                );
                mWriteIdAllocOp.createChunkServerAccessFlag =
                    mCSAccessExpireTime <= theNow;
                if (mAllocOp.allowCSClearTextFlag &&
                        theCSClearTextAllowedFlag &&
                        mWriteIdAllocOp.createChunkServerAccessFlag) {
                    mWriteIdAllocOp.decryptKey = &mChunkServer.GetSessionKey();
                }
                if (! mChunkServer.GetAuthContext()) {
                    mChunkServer.SetAuthContext(
                        mOuter.mMetaServer.GetAuthContext());
                }
            }
            if (mWriteIdAllocOp.status == 0) {
                const bool kCancelPendingOpsFlag = true;
                if (mChunkServer.SetServer(
                        mAllocOp.chunkServers[0],
                        kCancelPendingOpsFlag,
                        &mWriteIdAllocOp.statusMsg)) {
                    Enqueue(mWriteIdAllocOp);
                    return;
                }
                mWriteIdAllocOp.status = kErrorFault;
            }
            HandleError(mWriteIdAllocOp);
        }
        static int64_t GetAccessExpireTime(
            time_t  inNow,
            int64_t inIssedTime,
            int64_t inValidFor)
        {
            // Use current time if the clock difference is large enough.
            int64_t theDiff = inIssedTime - (int64_t)inNow;
            if (theDiff < 0) {
                theDiff = -theDiff;
            }
            return (
                ((LEASE_INTERVAL_SECS * 3 < theDiff) ? inNow : inIssedTime) +
                inValidFor - LEASE_INTERVAL_SECS
            );
        }
        void UpdateAccess(
            ChunkAccessOp& inOp)
        {
            if (! inOp.chunkAccessResponse.empty()) {
                mHasSubjectIdFlag      = true;
                mChunkAccess           = inOp.chunkAccessResponse;
                mChunkAccessExpireTime = GetAccessExpireTime(
                    Now(),
                    inOp.accessResponseIssued,
                    inOp.accessResponseValidForSec
                );
            }
            if (0 < inOp.accessResponseValidForSec &&
                    ! inOp.chunkServerAccessId.empty()) {
                mChunkServer.SetKey(
                    inOp.chunkServerAccessId.data(),
                    inOp.chunkServerAccessId.size(),
                    inOp.chunkServerAccessKey.GetPtr(),
                    inOp.chunkServerAccessKey.GetSize()
                );
                if (inOp.chunkAccessResponse.empty()) {
                    mCSAccessExpireTime = GetAccessExpireTime(
                        Now(),
                        inOp.accessResponseIssued,
                        inOp.accessResponseValidForSec
                    );
                } else {
                    mCSAccessExpireTime = mChunkAccessExpireTime;
                }
            }
        }
        void SetAccess(
            ChunkAccessOp& inOp,
            bool           inCanRequestAccessFlag = true)
        {
            const time_t theNow = Now();
            inOp.access                      = mChunkAccess;
            inOp.createChunkAccessFlag       = inCanRequestAccessFlag &&
                mChunkAccessExpireTime <= theNow;
            inOp.createChunkServerAccessFlag = inCanRequestAccessFlag &&
                mCSAccessExpireTime    <= theNow;
            inOp.hasSubjectIdFlag            =
                mHasSubjectIdFlag && ! mWriteIds.empty();
            if (inOp.hasSubjectIdFlag) {
                inOp.subjectId = mWriteIds.front().writeId;
            }
            if (inOp.createChunkServerAccessFlag &&
                    mChunkServer.IsShutdownSsl()) {
                inOp.decryptKey = &mChunkServer.GetSessionKey();
            }
            // Roll forward access time to indicate the request is in flight.
            // If op fails or times out, then write restarts from write id
            // allocation.
            if (inOp.createChunkAccessFlag) {
                mChunkAccessExpireTime = theNow + LEASE_INTERVAL_SECS * 3 / 2;
            }
            if (inOp.createChunkServerAccessFlag) {
                mCSAccessExpireTime = theNow + LEASE_INTERVAL_SECS * 3 / 2;
            }
        }
        void Done(
            WriteIdAllocOp& inOp,
            bool            inCanceledFlag,
            IOBuffer*       inBufferPtr)
        {
            QCASSERT(&mWriteIdAllocOp == &inOp && ! inBufferPtr);
            mWriteIds.clear();
            if (inCanceledFlag) {
                return;
            }
            if (0 <= inOp.status && inOp.chunkVersion < 0 &&
                    ! inOp.writePrepReplySupportedFlag) {
                // Chunk server / AP with object store support must have
                // write prepare reply support.
                inOp.status    = kErrorParameters;
                inOp.statusMsg = "invalid write id alloc reply: "
                    "write prepare reply is not supported";
            }
            if (inOp.status < 0) {
                HandleError(inOp);
                return;
            }
            const size_t theServerCount = inOp.chunkServerLoc.size();
            mWriteIds.reserve(theServerCount);
            const char*       thePtr    = inOp.writeIdStr.data();
            const char* const theEndPtr = thePtr + inOp.writeIdStr.size();
            for (size_t i = 0; i < theServerCount; i++) {
                WriteInfo theWInfo;
                if (! theWInfo.serverLoc.ParseString(
                        thePtr, theEndPtr - thePtr, inOp.shortRpcFormatFlag) ||
                    ! (inOp.shortRpcFormatFlag ?
                        HexIntParser::Parse(
                            thePtr, theEndPtr - thePtr, theWInfo.writeId) :
                        DecIntParser::Parse(
                            thePtr, theEndPtr - thePtr, theWInfo.writeId))) {
                    KFS_LOG_STREAM_ERROR << mLogPrefix <<
                        "write id alloc:"
                        " at index: "         << i <<
                        " of: "               << theServerCount <<
                        " invalid response: " << inOp.writeIdStr <<
                    KFS_LOG_EOM;
                    break;
                }
                mWriteIds.push_back(theWInfo);
            }
            if (theServerCount != mWriteIds.size()) {
                HandleError(inOp);
                return;
            }
            UpdateAccess(inOp);
            UpdateLeaseExpirationTime();
            StartWrite();
        }
        void Write()
        {
            if (mOpDoneFlagPtr) {
                return;
            }
            bool theOpDoneFlag = false;
            mOpDoneFlagPtr = &theOpDoneFlag;
            Queue::Iterator theIt(mPendingQueue);
            WriteOp* theOpPtr;
            while (! mSleepingFlag &&
                    mErrorCode == 0 &&
                    mAllocOp.chunkId > 0 &&
                    (theOpPtr = theIt.Next())) {
                Write(*theOpPtr);
                if (theOpDoneFlag) {
                    return; // Unwind. "this" might be deleted.
                }
            }
            mOpDoneFlagPtr = 0;
        }
        void Write(
            WriteOp& inWriteOp)
        {
            while (inWriteOp.mBeginBlock < inWriteOp.mEndBlock) {
                if (mInFlightBlocks.test(inWriteOp.mBeginBlock)) {
                    return; // Wait until the in flight write done.
                }
                mInFlightBlocks.set(inWriteOp.mBeginBlock++, 1);
            }
            Reset(inWriteOp);
            inWriteOp.contentLength =
                size_t(inWriteOp.mBuffer.BytesConsumable());
            inWriteOp.mWritePrepareOp.chunkId            = mAllocOp.chunkId;
            inWriteOp.mWritePrepareOp.chunkVersion       =
                mAllocOp.chunkVersion;
            inWriteOp.mWritePrepareOp.writeInfo          = mWriteIds;
            inWriteOp.mWritePrepareOp.contentLength      =
                inWriteOp.contentLength;
            inWriteOp.mWritePrepareOp.numBytes           =
                inWriteOp.contentLength;
            inWriteOp.mWritePrepareOp.replyRequestedFlag =
                mWriteIdAllocOp.writePrepReplySupportedFlag;
            // No need to recompute checksums on retry. Presently the buffer
            // remains the unchanged.
            SetAccess(
                inWriteOp.mWritePrepareOp,
                inWriteOp.mWritePrepareOp.replyRequestedFlag
            );
            if (inWriteOp.mWritePrepareOp.replyRequestedFlag) {
                if (! inWriteOp.mChecksumValidFlag) {
                    inWriteOp.mWritePrepareOp.checksum = ComputeBlockChecksum(
                        &inWriteOp.mBuffer,
                        inWriteOp.mWritePrepareOp.numBytes
                    );
                    inWriteOp.mChecksumValidFlag = true;
                }
                inWriteOp.mWritePrepareOp.checksums.clear();
            } else {
                if (inWriteOp.mWritePrepareOp.checksums.empty()) {
                    inWriteOp.mWritePrepareOp.checksums = ComputeChecksums(
                        &inWriteOp.mBuffer,
                        inWriteOp.mWritePrepareOp.numBytes,
                        &inWriteOp.mWritePrepareOp.checksum
                    );
                    inWriteOp.mChecksumValidFlag = true;
                }
                inWriteOp.mWriteSyncOp.chunkId      =
                    inWriteOp.mWritePrepareOp.chunkId;
                inWriteOp.mWriteSyncOp.chunkVersion =
                    inWriteOp.mWritePrepareOp.chunkVersion;
                inWriteOp.mWriteSyncOp.offset       =
                    inWriteOp.mWritePrepareOp.offset;
                inWriteOp.mWriteSyncOp.numBytes     =
                    inWriteOp.mWritePrepareOp.numBytes;
                inWriteOp.mWriteSyncOp.writeInfo    =
                    inWriteOp.mWritePrepareOp.writeInfo;
                inWriteOp.mWriteSyncOp.checksums    =
                    inWriteOp.mWritePrepareOp.checksums;
                SetAccess(inWriteOp.mWriteSyncOp);
            }
            inWriteOp.mOpStartTime = Now();
            Queue::Remove(mPendingQueue, inWriteOp);
            Queue::PushBack(mInFlightQueue, inWriteOp);
            mOuter.mStats.mOpsWriteCount++;
            mOuter.mStats.mOpsWriteByteCount += inWriteOp.contentLength;
            Enqueue(inWriteOp, &inWriteOp.mBuffer);
        }
        void Done(
            WriteOp&  inOp,
            bool      inCanceledFlag,
            IOBuffer* inBufferPtr)
        {
            QCASSERT(
                inBufferPtr == &inOp.mBuffer &&
                Queue::IsInList(mInFlightQueue, inOp)
            );
            inOp.InitBlockRange();
            for (size_t i = inOp.mBeginBlock; i < inOp.mEndBlock; i++) {
                mInFlightBlocks.set(i, 0);
            }
            if (inCanceledFlag || inOp.status < 0) {
                Queue::Remove(mInFlightQueue, inOp);
                Queue::PushBack(mPendingQueue, inOp);
                if (! inCanceledFlag) {
                    Monitor::ReportError(
                            Monitor::kWriteOpError,
                            mOuter.mMetaServer.GetMetaServerLocation(),
                            mChunkServer.GetServerLocation(),
                            inOp.status);
                    mOpStartTime = inOp.mOpStartTime;
                    HandleError(inOp);
                }
                return;
            }
            const Offset theOffset    = inOp.mWritePrepareOp.offset;
            const Offset theDoneCount = inOp.mBuffer.BytesConsumable();
            QCASSERT(
                theDoneCount >= 0 &&
                mPendingCount >= theDoneCount
            );
            mPendingCount -= theDoneCount;
            if (inOp.mWritePrepareOp.replyRequestedFlag) {
                UpdateAccess(inOp.mWritePrepareOp);
            } else {
                UpdateAccess(inOp.mWriteSyncOp);
            }
            inOp.Delete(mInFlightQueue);
            if (! ReportCompletion(theOffset, theDoneCount)) {
                return;
            }
            UpdateLeaseExpirationTime();
            StartWrite();
        }
        void UpdateLease()
        {
            QCASSERT(mWriteIdAllocOp.writePrepReplySupportedFlag &&
                0 < mAllocOp.chunkId && ! mWriteIds.empty());
            Reset(mUpdateLeaseOp);
            mUpdateLeaseOp.chunkId            = mAllocOp.chunkId;
            mUpdateLeaseOp.chunkVersion       = mAllocOp.chunkVersion;
            mUpdateLeaseOp.writeInfo          = mWriteIds;
            mUpdateLeaseOp.contentLength      = 0;
            mUpdateLeaseOp.numBytes           = 0;
            mUpdateLeaseOp.offset             = 0;
            mUpdateLeaseOp.checksum           = kKfsNullChecksum;
            mUpdateLeaseOp.replyRequestedFlag =
                mWriteIdAllocOp.writePrepReplySupportedFlag;
            mUpdateLeaseOp.checksums.clear();
            SetAccess(
                mUpdateLeaseOp,
                mUpdateLeaseOp.replyRequestedFlag
            );
            Enqueue(mUpdateLeaseOp);
        }
        void Done(
            WritePrepareOp& inOp,
            bool            inCanceledFlag,
            IOBuffer*       inBufferPtr)
        {
            QCASSERT(&mUpdateLeaseOp == &inOp && ! inBufferPtr);
            mUpdateLeaseOp.chunkId = -1;
            if (inCanceledFlag) {
                return;
            }
            if (0 != mUpdateLeaseOp.status) {
                HandleError(inOp);
                return;
            }
            if (mUpdateLeaseOp.replyRequestedFlag) {
                UpdateAccess(mUpdateLeaseOp);
            }
            UpdateLeaseExpirationTime();
            StartWrite();
        }
        void CloseChunk()
        {
            QCASSERT(mAllocOp.chunkId > 0);
            Reset(mCloseOp);
            mCloseOp.chunkId      = mAllocOp.chunkId;
            mCloseOp.chunkVersion = mAllocOp.chunkVersion;
            mCloseOp.writeInfo    = mWriteIds;
            if (mCloseOp.writeInfo.empty()) {
                mCloseOp.chunkServerLoc = mAllocOp.chunkServers;
            } else {
                mCloseOp.chunkServerLoc.clear();
            }
            SetAccess(mCloseOp);
            if (mCloseOp.chunkVersion < 0) {
                // Extend timeout to accommodate object commit, possibly single
                // atomic 64MB "object" write.
                const int theMaxWriteSize =
                    max(1 << 9, mOuter.mMaxWriteSize);
                const int theTimeout = min(LEASE_INTERVAL_SECS / 2,
                    (mOuter.mOpTimeoutSec + 3) / 4 *
                    (1 + max(mOuter.mMaxRetryCount / 3,
                    (int)((mMaxChunkPos + theMaxWriteSize - 1) /
                        theMaxWriteSize))));
                KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                    "chunk: "                << mCloseOp.chunkId <<
                    " version: "             << mCloseOp.chunkVersion <<
                    " chunk close timeout: " << theTimeout << " sec." <<
                KFS_LOG_EOM;
                mChunkServer.SetOpTimeoutSec(theTimeout);
            }
            mWriteIds.clear();
            mAllocOp.chunkId = -1;
            Enqueue(mCloseOp);
        }
        void Done(
            CloseOp&  inOp,
            bool      inCanceledFlag,
            IOBuffer* inBufferPtr)
        {
            QCASSERT(&mCloseOp == &inOp && ! inBufferPtr);
            if (mCloseOp.chunkVersion < 0) {
                // Restore timeout, changed by CloseChunk().
                mChunkServer.SetOpTimeoutSec(mOuter.mOpTimeoutSec);
            }
            if (inCanceledFlag) {
                return;
            }
            if (mCloseOp.status != 0) {
                if (mCloseOp.chunkVersion < 0) {
                    HandleError(inOp);
                    return;
                }
                KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                    "chunk close failure, status: " << mCloseOp.status <<
                    " ignored" <<
                KFS_LOG_EOM;
            }
            mKeepLeaseFlag = false;
            mCloseOp.chunkId = -1;
            Reset();
            StartWrite();
        }
        virtual void OpDone(
            KfsOp*    inOpPtr,
            bool      inCanceledFlag,
            IOBuffer* inBufferPtr)
        {
            if (mOpDoneFlagPtr) {
                *mOpDoneFlagPtr = true;
                mOpDoneFlagPtr = 0;
            }
            if (inOpPtr) {
                KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                    "<- " << (inCanceledFlag ? "canceled " : "") <<
                    inOpPtr->Show() <<
                    " status: " << inOpPtr->status <<
                    " msg: "    << inOpPtr->statusMsg <<
                    " seq: "    << inOpPtr->seq <<
                    " len: "    << inOpPtr->contentLength <<
                    " buffer: " << static_cast<const void*>(inBufferPtr) <<
                    "/" << (inBufferPtr ? inBufferPtr->BytesConsumable() : 0) <<
                KFS_LOG_EOM;
            } else {
                KFS_LOG_STREAM_ERROR << mLogPrefix <<
                    "<- " << (inCanceledFlag ? "canceled " : "") <<
                    "NULL operation completion?" <<
                    " buffer: " << static_cast<const void*>(inBufferPtr) <<
                    "/" << (inBufferPtr ? inBufferPtr->BytesConsumable() : 0) <<
                KFS_LOG_EOM;
            }
            if (inCanceledFlag && inOpPtr == &mAllocOp) {
                mOuter.mStats.mMetaOpsCancelledCount++;
            }
            if (mLastOpPtr == inOpPtr) {
                mLastOpPtr = 0;
            }
            if (&mAllocOp == inOpPtr) {
                Done(mAllocOp, inCanceledFlag, inBufferPtr);
            } else if (&mWriteIdAllocOp == inOpPtr) {
                Done(mWriteIdAllocOp, inCanceledFlag, inBufferPtr);
            } else if (&mAllocOp == inOpPtr) {
                Done(mAllocOp, inCanceledFlag, inBufferPtr);
            } else if (&mCloseOp == inOpPtr) {
                Done(mCloseOp, inCanceledFlag, inBufferPtr);
            } else if (&mUpdateLeaseOp == inOpPtr) {
                Done(mUpdateLeaseOp, inCanceledFlag, inBufferPtr);
            } else if (inOpPtr && inOpPtr->op == CMD_WRITE) {
                Done(*static_cast<WriteOp*>(inOpPtr),
                    inCanceledFlag, inBufferPtr);
            } else {
                mOuter.InternalError("unexpected operation completion");
            }
        }
        void Enqueue(
            KfsOp&    inOp,
            IOBuffer* inBufferPtr = 0)
            { EnqueueSelf(inOp, inBufferPtr, &mChunkServer, 0); }
        void EnqueueMeta(
            KfsOp&    inOp,
            IOBuffer* inBufferPtr    = 0,
            int       inExtraTimeout = 0)
            { EnqueueSelf(inOp, inBufferPtr, 0, inExtraTimeout); }
        void Reset()
        {
            if (mLastOpPtr == &mAllocOp) {
                mOuter.mMetaServer.Cancel(mLastOpPtr, this);
            }
            Reset(mAllocOp);
            mWriteIds.clear();
            mAllocOp.chunkId = 0;
            mLastOpPtr       = 0;
            mChunkServer.Stop();
            QCASSERT(Queue::IsEmpty(mInFlightQueue));
            if (mSleepingFlag) {
                mSleepTimer.RemoveTimeout();
                mSleepingFlag = false;
            }
            mLeaseUpdatePendingFlag = false;
        }
        static void Reset(
            KfsOp& inOp)
        {
            inOp.seq           = 0;
            inOp.status        = 0;
            inOp.lastError     = 0;
            inOp.statusMsg.clear();
            inOp.checksum      = 0;
            inOp.contentLength = 0;
            inOp.DeallocContentBuf();
        }
        static void Reset(
            ChunkAccessOp& inOp)
        {
            KfsOp& theKfsOp = inOp;
            Reset(theKfsOp);
            inOp.access.clear();
            inOp.createChunkAccessFlag       = false;
            inOp.createChunkServerAccessFlag = false;
            inOp.hasSubjectIdFlag            = false;
            inOp.subjectId                   = -1;
            inOp.accessResponseValidForSec   = 0;
            inOp.accessResponseIssued        = 0;
            inOp.chunkAccessResponse.clear();
            inOp.chunkServerAccessId.clear();
            inOp.decryptKey                  = 0;
        }
        int GetTimeToNextRetry() const
        {
            return max(mRetryCount >= 1 ? 1 : 0,
                mOuter.mTimeSecBetweenRetries - int(Now() - mOpStartTime));
        }
        void HandleError(
            KfsOp& inOp)
        {
            ostringstream theOStream;
            ReqOstream theStream(theOStream);
            inOp.Request(theStream);
            KFS_LOG_STREAM_ERROR << mLogPrefix <<
                "operation"
                " failure, seq: "         << inOp.seq       <<
                " status: "               << inOp.status    <<
                " msg: "                  << inOp.statusMsg <<
                " op: "                   << inOp.Show()    <<
                " current chunk server: " << mChunkServer.GetServerLocation() <<
                " chunkserver: "          << (mChunkServer.IsDataSent() ?
                    (mChunkServer.IsAllDataSent() ? "all" : "partial") :
                    "no") << " data sent" <<
                "\nRequest:\n"            << theOStream.str() <<
            KFS_LOG_EOM;
            int       theStatus    = inOp.status;
            const int theLastError = inOp.lastError;
            if (&inOp == &mAllocOp) {
                if (theStatus == kErrorNoEntry) {
                    // File deleted, and lease expired or meta server restarted.
                    KFS_LOG_STREAM_ERROR << mLogPrefix <<
                        "file does not exist, giving up" <<
                    KFS_LOG_EOM;
                    mErrorCode = theStatus;
                    Reset();
                    mOuter.FatalError(theStatus);
                    return;
                }
                if (theStatus == kErrorReadOnly && mClosingFlag &&
                        0 < mCloseOp.chunkId && mKeepLeaseFlag) {
                    KFS_LOG_STREAM_ERROR << mLogPrefix <<
                        "object store block is now stable stable" <<
                    KFS_LOG_EOM;
                    mKeepLeaseFlag = false;
                    mCloseOp.chunkId = -1;
                    Reset();
                    StartWrite();
                    return;
                    /*
                    Although it might be possible to verify that the block is
                    stable by using the following code, the problem is that the
                    block (chunk) and chunk server access might have expired
                    already, and the only way to obtain the access is successful
                    block allocation completion.
                    Reset(mAllocOp);
                    mAllocOp.chunkId      = mCloseOp.chunkId;
                    mAllocOp.chunkVersion = mCloseOp.chunkVersion;
                    mWriteIds             = mCloseOp.writeInfo;
                    StartWrite();
                    return;
                    */
                }
                if (KfsNetClient::kErrorMaxRetryReached == theStatus &&
                        mRetryCount < mOuter.mMaxRetryCount) {
                    // Meta server state machine all connection attempts have
                    // failed.
                    mRetryCount = mOuter.mMaxRetryCount;
                }
            }
            if (mOuter.mStriperPtr && ! mAllocOp.invalidateAllFlag &&
                    mAllocOp.fileOffset >= 0 &&
                    ! mOuter.mStriperPtr->IsWriteRetryNeeded(
                        mAllocOp.fileOffset,
                        mRetryCount,
                        mOuter.mMaxRetryCount,
                        theStatus)) {
                KFS_LOG_STREAM_INFO << mLogPrefix <<
                    "invalidate:"
                    " chunk: "   << mAllocOp.chunkId <<
                    " offset: "  << mAllocOp.fileOffset <<
                    " status: "  << inOp.status <<
                    " => "       << theStatus <<
                    " pending: " << mPendingCount <<
                    " w-empty: " << Queue::IsEmpty(mPendingQueue) <<
                KFS_LOG_EOM;
                mErrorCode = theStatus;
                mAllocOp.invalidateAllFlag = true;
                mRetryCount = 0;
                Reset();
                QCASSERT(CanWrite());
                StartWrite();
                return;
            }
            if (++mRetryCount > mOuter.mMaxRetryCount) {
                KFS_LOG_STREAM_ERROR << mLogPrefix <<
                    "max retry reached: " << mRetryCount << ", giving up" <<
                KFS_LOG_EOM;
                if (0 <= theStatus) {
                    theStatus = kErrorIo;
                } else if (KfsNetClient::kErrorMaxRetryReached == theStatus &&
                        theLastError < 0) {
                    theStatus = theLastError;
                }
                mErrorCode = theStatus;
                Reset();
                mOuter.FatalError(theStatus);
                return;
            }
            // Treat alloc failure the same as chunk server failure.
            if (&mAllocOp == mLastOpPtr) {
                mOuter.mStats.mAllocRetriesCount++;
            }
            mOuter.mStats.mRetriesCount++;
            int theTimeToNextRetry = GetTimeToNextRetry();
            if (mKeepLeaseFlag) {
                theTimeToNextRetry = (int)min(
                    max(mRetryCount <= 1 ? (time_t)0 : (time_t)max(2,
                        LEASE_INTERVAL_SECS /
                            (2 * max(1, mOuter.mMaxRetryCount))),
                        mLeaseExpireTime - Now()),
                    (time_t)theTimeToNextRetry
                );
            }
            // Retry.
            KFS_LOG_STREAM_INFO << mLogPrefix <<
                "scheduling retry: " << mRetryCount <<
                " of "  << mOuter.mMaxRetryCount <<
                " in "  << theTimeToNextRetry << " sec." <<
                " op: " << inOp.Show() <<
            KFS_LOG_EOM;
            mErrorCode = 0;
            Reset();
            Sleep(theTimeToNextRetry);
            if (! mSleepingFlag) {
               Timeout();
            }
        }
        bool Sleep(
            int inSec)
        {
            if (inSec <= 0 || mSleepingFlag) {
                return false;
            }
            KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                "sleeping: " << inSec <<
            KFS_LOG_EOM;
            mSleepingFlag = true;
            mOuter.mStats.mSleepTimeSec += inSec;
            mSleepTimer.SetTimeout(inSec);
            return true;
        }
        void Timeout()
        {
            KFS_LOG_STREAM_DEBUG << mLogPrefix << "timeout" <<
            KFS_LOG_EOM;
            if (mSleepingFlag) {
                mSleepTimer.RemoveTimeout();
                mSleepingFlag = false;
            }
            StartWrite();
        }
        int EventHandler(
            int   inCode,
            void* inDataPtr)
        {
            QCRTASSERT(EVENT_INACTIVITY_TIMEOUT == inCode && ! inDataPtr);
            Timeout();
            return 0;
        }
        bool ReportCompletion(
            Offset inOffset = 0,
            Offset inSize   = 0)
        {
            if (mErrorCode == 0) {
                // Reset retry counts on successful completion.
                mRetryCount = 0;
            }
            return mOuter.ReportCompletion(this, inOffset, inSize);
        }
        time_t Now() const
            { return mOuter.mNetManager.Now(); }
        void EnqueueSelf(
            KfsOp&        inOp,
            IOBuffer*     inBufferPtr,
            KfsNetClient* inServerPtr,
            int           inExtraTimeout)
        {
            mLastOpPtr   = &inOp;
            mOpStartTime = Now();
            KFS_LOG_STREAM_DEBUG << mLogPrefix <<
                "+> " << (inServerPtr ? "" : "meta ") << inOp.Show() <<
                " buffer: " << (void*)inBufferPtr <<
                "/" << (inBufferPtr ? inBufferPtr->BytesConsumable() : 0) <<
            KFS_LOG_EOM;
            if (inServerPtr) {
                mOuter.mStats.mChunkOpsQueuedCount++;
            } else {
                mOuter.mStats.mMetaOpsQueuedCount++;
            }
            if (! (inServerPtr ? *inServerPtr : mOuter.mMetaServer).Enqueue(
                    &inOp, this, inBufferPtr, inExtraTimeout)) {
                mOuter.InternalError(inServerPtr ?
                    "chunk op enqueue failure" :
                    "meta op enqueue failure"
                );
                inOp.status = kErrorFault;
                OpDone(&inOp, false, inBufferPtr);
            }
        }
    private:
        ChunkWriter(
            const ChunkWriter& inChunkWriter);
        ChunkWriter& operator=(
            const ChunkWriter& inChunkWriter);
    };
    friend class ChunkWriter;
    friend class Striper;

    typedef ChunkWriter::Writers Writers;

    Writer&             mOuter;
    MetaServer&         mMetaServer;
    string              mPathName;
    kfsFileId_t         mFileId;
    bool                mClosingFlag;
    bool                mSleepingFlag;
    int                 mErrorCode;
    int                 mWriteThreshold;
    int                 mPartialBuffersCount;
    Offset              mPendingCount;
    const int           mIdleTimeoutSec;
    const int           mOpTimeoutSec;
    const int           mMaxRetryCount;
    const int           mTimeSecBetweenRetries;
    const int           mMaxPartialBuffersCount;
    const int           mMaxWriteSize;
    Offset              mMaxPendingThreshold;
    int                 mReplicaCount;
    int                 mRetryCount;
    Offset              mFileSize;
    Offset              mOffset;
    Offset              mOpenChunkBlockSize;
    int64_t             mChunkServerInitialSeqNum;
    Completion*         mCompletionPtr;
    IOBuffer            mBuffer;
    string const        mLogPrefix;
    Stats               mStats;
    KfsNetClient::Stats mChunkServersStats;
    NetManager&         mNetManager;
    TruncateOp          mTruncateOp;
    time_t              mOpStartTime;
    int                 mCompletionDepthCount;
    int                 mStriperProcessCount;
    Striper*            mStriperPtr;
    ChunkWriter*        mWriters[1];

    void InternalError(
            const char* inMsgPtr = 0)
    {
        if (inMsgPtr) {
            KFS_LOG_STREAM_FATAL << inMsgPtr << KFS_LOG_EOM;
        }
        MsgLogger::Stop();
        abort();
    }

    virtual ~Impl()
    {
        Impl::DisableCompletion();
        Impl::Shutdown();
        delete mStriperPtr;
    }
    Offset GetPendingSizeSelf() const
    {
        return (mBuffer.BytesConsumable() +
            (mStriperPtr ?
                max(Offset(0), mStriperPtr->GetPendingSize()) :
                Offset(0)
        ));
    }
    int StartWrite(
        bool inFlushFlag = false)
    {
        KFS_LOG_STREAM_DEBUG << mLogPrefix <<
            "start write:" <<
            " offset: "  << mOffset <<
            " pending: " << GetPendingSizeSelf() <<
            " / "        << mBuffer.BytesConsumable() <<
            " thresh: "  << mWriteThreshold <<
            " / "        << mMaxPendingThreshold <<
            " flush: "   << inFlushFlag <<
            (mSleepingFlag ? " SLEEPING" : "") <<
        KFS_LOG_EOM;

        if (mSleepingFlag) {
            return mErrorCode;
        }
        const bool   theFlushFlag           = inFlushFlag || mClosingFlag;
        const Offset theWriteThreshold      =
            max(1, theFlushFlag ? 1 : mWriteThreshold);
        const Offset theQueueWriteThreshold =
            min(mMaxPendingThreshold, theWriteThreshold);
        while (mErrorCode == 0 && (
                mMaxPendingThreshold <= mBuffer.BytesConsumable() ||
                theWriteThreshold <= GetPendingSizeSelf())) {
            const int thePrevRefCount = GetRefCount();
            QueueWrite(theQueueWriteThreshold);
            if (thePrevRefCount > GetRefCount()) {
                return mErrorCode; // Unwind
            }
            if (mBuffer.IsEmpty()) {
                break;
            }
        }
        if (! mClosingFlag) {
            return mErrorCode;
        }
        if (Writers::IsEmpty(mWriters)) {
            ReportCompletion();
            return mErrorCode;
        }
        Writers::Iterator theIt(mWriters);
        ChunkWriter*      thePtr;
        while ((thePtr = theIt.Next())) {
            if (! thePtr->IsOpen()) {
                continue;
            }
            const int thePrevRefCount = GetRefCount();
            thePtr->Close();
            if (thePrevRefCount > GetRefCount()) {
                return mErrorCode; // Unwind
            }
            // Restart from the beginning as close can invoke completion
            // and remove or close more than one writer in TryToCloseIdle().
            theIt.Reset();
        }
        if (Writers::IsEmpty(mWriters) && mClosingFlag) {
            SetFileSize();
        }
        return mErrorCode;
    }
    void SetFileSize()
    {
        if ((! mStriperPtr && 0 != mReplicaCount) ||
                mErrorCode != 0 || 0 <= mTruncateOp.fid) {
            return;
        }
        const Offset theSize = mStriperPtr ?
            mStriperPtr->GetFileSize() :
            mOffset + mBuffer.BytesConsumable();
        if (theSize < 0 || theSize <= mTruncateOp.fileOffset) {
            return;
        }
        mOpStartTime           = mNetManager.Now();
        mTruncateOp.pathname   = mPathName.c_str();
        mTruncateOp.fid        = mFileId;
        mTruncateOp.fileOffset = theSize;
        mTruncateOp.status     = 0;
        KFS_LOG_STREAM_DEBUG << mLogPrefix <<
            "meta +> " << mTruncateOp.Show() <<
        KFS_LOG_EOM;
        if (! mMetaServer.Enqueue(&mTruncateOp, this, 0)) {
            InternalError("meta truncate enqueue failure");
            mTruncateOp.status = kErrorFault;
            OpDone(&mTruncateOp, false, 0);
        }
    }
    virtual void OpDone(
        KfsOp*    inOpPtr,
        bool      inCanceledFlag,
        IOBuffer* inBufferPtr)
    {
        KFS_LOG_STREAM_DEBUG << mLogPrefix <<
            "meta <- " << (inOpPtr ? inOpPtr->Show() : kKfsNullOp.Show()) <<
            (inCanceledFlag ? " canceled" : "") <<
            " status: " << (inOpPtr ? inOpPtr->status : 0) <<
            " " << (inOpPtr ? inOpPtr->statusMsg : string()) <<
        KFS_LOG_EOM;
        QCASSERT(inOpPtr == &mTruncateOp);
        if (inOpPtr != &mTruncateOp) {
            return;
        }
        mTruncateOp.pathname = 0;
        mTruncateOp.fid      = -1;
        if (inCanceledFlag) {
            mTruncateOp.fileOffset = -1;
            return;
        }
        if (mTruncateOp.status != 0) {
            KFS_LOG_STREAM_ERROR << mLogPrefix <<
                "set size failure:"
                " offset: " << mTruncateOp.fileOffset <<
                " status: " << mTruncateOp.status <<
                " msg: "    << mTruncateOp.statusMsg <<
                " retry: "  << mRetryCount <<
                " of: "     << mMaxRetryCount <<
            KFS_LOG_EOM;
            mTruncateOp.fileOffset = -1;
            if (++mRetryCount < mMaxRetryCount) {
                Sleep(max(
                    mRetryCount > 1 ? 1 : 0,
                    mTimeSecBetweenRetries -
                        int(mNetManager.Now() - mOpStartTime)
                ));
                if (! mSleepingFlag) {
                    StartWrite();
                }
            } else {
                FatalError(
                    (KfsNetClient::kErrorMaxRetryReached == mTruncateOp.status
                        && mTruncateOp.lastError < 0) ?
                    mTruncateOp.lastError : mTruncateOp.status
                );
            }
        } else {
            mRetryCount = 0;
            ReportCompletion();
        }
    }
    bool Sleep(
        int inSec)
    {
        if (inSec <= 0 || mSleepingFlag) {
            return false;
        }
        KFS_LOG_STREAM_DEBUG << mLogPrefix <<
            "sleeping: " << inSec <<
        KFS_LOG_EOM;
        mSleepingFlag = true;
        mStats.mSleepTimeSec += inSec;
        const bool kResetTimerFlag = true;
        SetTimeoutInterval(inSec * 1000, kResetTimerFlag);
        mNetManager.RegisterTimeoutHandler(this);
        return true;
    }
    virtual void Timeout()
    {
        KFS_LOG_STREAM_DEBUG << mLogPrefix << "timeout" <<
        KFS_LOG_EOM;
        if (mSleepingFlag) {
            mNetManager.UnRegisterTimeoutHandler(this);
            mSleepingFlag = false;
        }
        StartWrite();
    }
    void QueueWrite(
        Offset inWriteThreshold)
    {
        if (mStriperPtr) {
            QCStValueIncrementor<int> theIncrement(mStriperProcessCount, 1);
            const int theErrCode =
                mStriperPtr->Process(mBuffer, mOffset, inWriteThreshold);
            if (theErrCode != 0 && mErrorCode == 0) {
                mErrorCode = theErrCode;
            }
            return;
        }
        const Offset theQueuedCount = QueueWrite(
            mBuffer,
            mBuffer.BytesConsumable(),
            mOffset,
            inWriteThreshold
        );
        if (theQueuedCount > 0) {
            mOffset += theQueuedCount;
            StartQueuedWrite(theQueuedCount);
        }
    }
    Offset QueueWrite(
        IOBuffer& inBuffer,
        Offset    inSize,
        Offset    inOffset,
        int       inWriteThreshold)
    {
        QCASSERT(inOffset >= 0);
        if (inSize <= 0 || inBuffer.BytesConsumable() <= 0) {
            return 0;
        }
        const Offset theFileOffset = inOffset - inOffset % CHUNKSIZE;
        Writers::Iterator theIt(mWriters);
        ChunkWriter* thePtr;
        while ((thePtr = theIt.Next())) {
            if (thePtr->GetFileOffset() == theFileOffset) {
                break;
            }
        }
        if (thePtr) {
            Writers::PushFront(mWriters, *thePtr);
            thePtr->CancelClose();
        } else {
            mChunkServerInitialSeqNum += 10000;
            thePtr = new ChunkWriter(
                *this, mChunkServerInitialSeqNum, mLogPrefix);
        }
        const Offset theQueuedCount = thePtr->QueueWrite(
            inBuffer, inSize, inOffset, inWriteThreshold);
        QCASSERT(Writers::Front(mWriters) == thePtr);
        return theQueuedCount;
    }
    void StartQueuedWrite(
        Offset inQueuedCount)
    {
        if (inQueuedCount <= 0) {
            return;
        }
        QCASSERT(! Writers::IsEmpty(mWriters));
        mPendingCount += inQueuedCount;
        Writers::Front(mWriters)->StartWrite();
    }
    void FatalError(
        int inErrorCode = 0)
    {
        if (mErrorCode == 0) {
            mErrorCode = inErrorCode;
        }
        if (mErrorCode == 0) {
            mErrorCode = kErrorIo;
        }
        mClosingFlag = false;
        ReportCompletion();
    }
    bool CanClose(
        ChunkWriter& inWriter)
    {
        if (! inWriter.IsIdle()) {
            return false;
        }
        if (! inWriter.IsOpen() || mClosingFlag) {
            return true;
        }
        // The most recently used should always be first.
        const ChunkWriter* const thePtr = Writers::Front(mWriters);
        if (! thePtr) {
            return true;
        }
        // With object store files close even a single chunk writer as soon as
        // chunk is complete as re-write is not supported, in order to minimize
        // the number of non-stable chunks, and the corresponding memory
        // buffers.
        if (0 < mReplicaCount && thePtr == &inWriter) {
            return false;
        }
        const Offset theLeftEdge = thePtr->GetOpenChunkBlockFileOffset();
        if (theLeftEdge < 0) {
            return false;
        }
        const Offset theRightEdge = theLeftEdge + mOpenChunkBlockSize;
        const Offset theOffset    = inWriter.GetFileOffset();
        return (theOffset < theLeftEdge || theRightEdge <= theOffset);
    }
    bool TryToCloseIdle(
        const ChunkWriter* inWriterPtr)
    {
        ChunkWriter* thePtr = Writers::Back(mWriters);
        if (! thePtr) {
            return (! inWriterPtr); // Already deleted.
        }
        bool theRetFlag = true;
        while (thePtr) {
            ChunkWriter& theWriter = *thePtr;
            thePtr = (thePtr == Writers::Front(mWriters)) ?
                0 : theWriter.GetPrevPtr();
            if (CanClose(theWriter)) {
                const bool theOpenFlag = theWriter.IsOpen();
                if (theOpenFlag) {
                    theWriter.Close();
                }
                // Handle "synchronous" Close(). ReportCompletion, calls
                // this method only when mCompletionDepthCount <= 1
                if (! theOpenFlag ||
                        (! theWriter.IsOpen() && CanClose(theWriter))) {
                    if (&theWriter == inWriterPtr) {
                        theRetFlag = false;
                    }
                    delete &theWriter;
                }
            } else if (theWriter.IsIdle() && theWriter.IsOpen()) {
                // Stop at the first idle that can not be closed.
                break;
            }
        }
        return theRetFlag;
    }
    bool ReportCompletion(
        ChunkWriter* inWriterPtr = 0,
        Offset       inOffset    = 0,
        Offset       inSize      = 0)
    {
        // Order matters here, as StRef desctructor can delete this.
        StRef                     theRef(*this);
        QCStValueIncrementor<int> theIncrement(mCompletionDepthCount, 1);

        QCRTASSERT(mPendingCount >= 0 && mPendingCount >= inSize);
        mPendingCount -= inSize;
        if (inWriterPtr && mErrorCode == 0) {
            mErrorCode = inWriterPtr->GetErrorCode();
        }
        const int thePrevRefCount = GetRefCount();
        if (mCompletionPtr) {
            mCompletionPtr->Done(mOuter, mErrorCode, inOffset, inSize);
        }
        bool theRet = true;
        if (mCompletionDepthCount <= 1 && mStriperProcessCount <= 0) {
            theRet = TryToCloseIdle(inWriterPtr);
            if (mClosingFlag && Writers::IsEmpty(mWriters) && ! mSleepingFlag) {
                SetFileSize();
                if (mTruncateOp.fid < 0 && ! mSleepingFlag) {
                    mClosingFlag = false;
                    mFileId = -1;
                    Striper* const theStriperPtr = mStriperPtr;
                    mStriperPtr = 0;
                    delete theStriperPtr;
                    theRet = false;
                    if (mCompletionPtr) {
                        mCompletionPtr->Done(mOuter, mErrorCode, 0, 0);
                    }
                }
            }
        }
        return (theRet && thePrevRefCount <= GetRefCount());
    }
    bool IsChunkServerClearTextAllowed()
    {
        ClientAuthContext* const theCtxPtr = mMetaServer.GetAuthContext();
        return (! theCtxPtr || theCtxPtr->IsChunkServerClearTextAllowed());
    }
private:
    Impl(
        const Impl& inWriter);
    Impl& operator=(
        const Impl& inWriter);
};

/* static */ Writer::Striper*
Writer::Striper::Create(
    int                      inType,
    int                      inStripeCount,
    int                      inRecoveryStripeCount,
    int                      inStripeSize,
    Writer::Striper::Offset  inFileSize,
    const string&            inLogPrefix,
    Writer::Striper::Impl&   inOuter,
    Writer::Striper::Offset& outOpenChunkBlockSize,
    string&                  outErrMsg)
{
    switch (inType) {
        case KFS_STRIPED_FILE_TYPE_NONE:
            outOpenChunkBlockSize = Offset(CHUNKSIZE);
        break;
        default:
            return RSStriperCreate(
                inType,
                inStripeCount,
                inRecoveryStripeCount,
                inStripeSize,
                inFileSize,
                inLogPrefix,
                inOuter,
                outOpenChunkBlockSize,
                outErrMsg
            );
    }
    return 0;
}

Writer::Offset
Writer::Striper::QueueWrite(
    IOBuffer&       inBuffer,
    Writer::Offset  inSize,
    Writer::Offset  inOffset,
    int             inWriteThreshold)
{
    const Offset theQueuedCount = mOuter.QueueWrite(
        inBuffer, inSize, inOffset, inWriteThreshold);
    mWriteQueuedFlag = theQueuedCount > 0;
    return theQueuedCount;
}

void
Writer::Striper::StartQueuedWrite(
    Writer::Offset inQueuedCount)
{
    if (! mWriteQueuedFlag) {
        return;
    }
    mWriteQueuedFlag = false;
    mOuter.StartQueuedWrite(inQueuedCount);
}

Writer::Writer(
    Writer::MetaServer& inMetaServer,
    Writer::Completion* inCompletionPtr,
    int                 inMaxRetryCount,
    int                 inWriteThreshold,
    int                 inMaxPartialBuffersCount,
    int                 inTimeSecBetweenRetries,
    int                 inOpTimeoutSec,
    int                 inIdleTimeoutSec,
    int                 inMaxWriteSize,
    const char*         inLogPrefixPtr,
    int64_t             inChunkServerInitialSeqNum)
    : mImpl(*new Writer::Impl(
        *this,
        inMetaServer,
        inCompletionPtr,
        inMaxRetryCount,
        inWriteThreshold,
        inMaxPartialBuffersCount,
        inTimeSecBetweenRetries,
        inOpTimeoutSec,
        inIdleTimeoutSec,
        inMaxWriteSize,
        (inLogPrefixPtr && inLogPrefixPtr[0]) ?
            (inLogPrefixPtr + string(" ")) : string(),
        inChunkServerInitialSeqNum
    ))
{
    mImpl.Ref();
}

/* virtual */
Writer::~Writer()
{
    mImpl.DisableCompletion();
    mImpl.UnRef();
}

int
Writer::Open(
    kfsFileId_t inFileId,
    const char* inFileNamePtr,
    Offset      inFileSize,
    int         inStriperType,
    int         inStripeSize,
    int         inStripeCount,
    int         inRecoveryStripeCount,
    int         inReplicaCount)
{
    Impl::StRef theRef(mImpl);
    return mImpl.Open(
        inFileId,
        inFileNamePtr,
        inFileSize,
        inStriperType,
        inStripeSize,
        inStripeCount,
        inRecoveryStripeCount,
        inReplicaCount
    );
}

int
Writer::Close()
{
    Impl::StRef theRef(mImpl);
    return mImpl.Close();
}

Writer::Offset
Writer::Write(
    IOBuffer&      inBuffer,
    Writer::Offset inLength,
    Writer::Offset inOffset,
    bool           inFlushFlag,
    int            inWriteThreshold)
{
    Impl::StRef theRef(mImpl);
    return mImpl.Write(
        inBuffer, inLength, inOffset, inFlushFlag, inWriteThreshold);
}

bool
Writer::IsOpen() const
{
    Impl::StRef theRef(mImpl);
    return (mImpl.IsOpen() && ! IsClosing());
}

bool
Writer::IsClosing() const
{
    Impl::StRef theRef(mImpl);
    return mImpl.IsClosing();
}

bool
Writer::IsActive() const
{
    Impl::StRef theRef(mImpl);
    return mImpl.IsActive();
}

Writer::Offset
Writer::GetPendingSize() const
{
    Impl::StRef theRef(mImpl);
    return mImpl.GetPendingSize();
}

int
Writer::GetErrorCode() const
{
    Impl::StRef theRef(mImpl);
    return mImpl.GetErrorCode();
}

int
Writer::SetWriteThreshold(
    int inThreshold)
{
    Impl::StRef theRef(mImpl);
    return mImpl.SetWriteThreshold(inThreshold);
}

int
Writer::Flush()
{
    Impl::StRef theRef(mImpl);
    return mImpl.Flush();
}

void
Writer::Stop()
{
    Impl::StRef theRef(mImpl);
    mImpl.Stop();
}

void
Writer::Shutdown()
{
    Impl::StRef theRef(mImpl);
    mImpl.Shutdown();
}

void
Writer::Register(
    Writer::Completion* inCompletionPtr)
{
    Impl::StRef theRef(mImpl);
    mImpl.Register(inCompletionPtr);
}

bool
Writer::Unregister(
   Writer::Completion* inCompletionPtr)
{
    Impl::StRef theRef(mImpl);
    return mImpl.Unregister(inCompletionPtr);
}

void
Writer::GetStats(
    Stats&               outStats,
    KfsNetClient::Stats& outChunkServersStats) const
{
    Impl::StRef theRef(mImpl);
    mImpl.GetStats(outStats, outChunkServersStats);
}

}}
