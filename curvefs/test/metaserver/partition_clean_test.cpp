/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * @Project: curve
 * @Date: 2021-12-23 16:16:52
 * @Author: chenwei
 */
#include <gtest/gtest.h>

#include "curvefs/src/metaserver/partition.h"
#include "curvefs/src/metaserver/partition_clean_manager.h"
#include "curvefs/src/metaserver/partition_cleaner.h"
#include "curvefs/test/metaserver/copyset/mock/mock_copyset_node.h"
#include "curvefs/test/client/rpcclient/mock_mds_client.h"
#include "curvefs/test/metaserver/mock_metaserver_s3_adaptor.h"
#include "curvefs/src/metaserver/storage/storage.h"
#include "curvefs/src/metaserver/storage/rocksdb_storage.h"
#include "curvefs/test/metaserver/storage/utils.h"
#include "curvefs/src/metaserver/mds/fsinfo_manager.h"
#include "src/fs/ext4_filesystem_impl.h"

using ::curvefs::client::rpcclient::MockMdsClient;
using ::curvefs::mds::FSStatusCode;
using ::testing::_;
using ::testing::Invoke;
using ::testing::Matcher;
using ::testing::Return;

using ::curvefs::metaserver::storage::KVStorage;
using ::curvefs::metaserver::storage::RandomStoragePath;
using ::curvefs::metaserver::storage::RocksDBStorage;
using ::curvefs::metaserver::storage::StorageOptions;
using ::curvefs::metaserver::copyset::MockCopysetNode;

namespace curvefs {
namespace metaserver {

namespace {
auto localfs = curve::fs::Ext4FileSystemImpl::getInstance();
}

class PartitionCleanManagerTest : public testing::Test {
 protected:
    void SetUp() override {
        dataDir_ = RandomStoragePath();
        StorageOptions options;
        options.dataDir = dataDir_;
        options.localFileSystem = localfs.get();
        kvStorage_ = std::make_shared<RocksDBStorage>(options);
        ASSERT_TRUE(kvStorage_->Open());

        mdsCli_ = std::make_shared<MockMdsClient>();
        s3Adaptor_ = std::make_shared<MockS3ClientAdaptor>();
        copyset_ = new MockCopysetNode();
        FsInfoManager::GetInstance().SetMdsClient(mdsCli_);
    }

    void TearDown() override {
        ASSERT_TRUE(kvStorage_->Close());
        auto output = execShell("rm -rf " + dataDir_);
        ASSERT_EQ(output.size(), 0);

        delete copyset_;
    }

    std::string execShell(const string &cmd) {
        std::array<char, 128> buffer;
        std::string result;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                      pclose);
        if (!pipe) {
            throw std::runtime_error("popen() failed!");
        }
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        return result;
    }

 protected:
    std::string dataDir_;
    std::shared_ptr<KVStorage> kvStorage_;
    std::shared_ptr<MockMdsClient> mdsCli_;
    std::shared_ptr<MockS3ClientAdaptor> s3Adaptor_;
    MockCopysetNode *copyset_;
};

TEST_F(PartitionCleanManagerTest, test1) {
    ASSERT_TRUE(true);
    PartitionCleanManager *manager = &PartitionCleanManager::GetInstance();

    PartitionCleanOption option;
    option.scanPeriodSec = 1;
    option.inodeDeletePeriodMs = 500;
    option.s3Adaptor = s3Adaptor_;
    option.mdsClient = mdsCli_;
    manager->Init(option);
    manager->Run();
    uint32_t partitionId = 1;
    uint32_t fsId = 2;
    PartitionInfo partitionInfo;
    partitionInfo.set_partitionid(partitionId);
    partitionInfo.set_fsid(fsId);
    partitionInfo.set_start(0);
    partitionInfo.set_end(2000);
    std::shared_ptr<Partition> partition =
        std::make_shared<Partition>(partitionInfo, kvStorage_);
    Dentry dentry;
    dentry.set_fsid(fsId);
    dentry.set_parentinodeid(1);

    InodeParam param;
    param.fsId = fsId;
    param.gid = 0;
    param.uid = 0;
    param.mode = 0;
    param.type = FsFileType::TYPE_DIRECTORY;
    param.length = 0;
    param.rdev = 0;

    dentry.set_name("/");
    dentry.set_inodeid(1100);
    dentry.set_txid(0);
    dentry.set_type(FsFileType::TYPE_DIRECTORY);
    ASSERT_EQ(partition->CreateRootInode(param), MetaStatusCode::OK);
    ASSERT_EQ(partition->CreateDentry(dentry), MetaStatusCode::OK);
    ASSERT_EQ(partition->CreateDentry(dentry), MetaStatusCode::OK);

    Inode inode1;
    param.type = FsFileType::TYPE_S3;
    param.symlink = "";
    ASSERT_EQ(partition->CreateInode(param, &inode1), MetaStatusCode::OK);
    ASSERT_EQ(partition->GetInodeNum(), 2);
    ASSERT_EQ(partition->GetDentryNum(), 1);
    Inode inode2;
    ASSERT_EQ(partition->GetInode(fsId, ROOTINODEID, &inode2),
              MetaStatusCode::OK);

    std::shared_ptr<PartitionCleaner> partitionCleaner =
        std::make_shared<PartitionCleaner>(partition);

    EXPECT_CALL(*copyset_, IsLeaderTerm())
        .WillOnce(Return(false))
        .WillRepeatedly(Return(true));

    EXPECT_CALL(*copyset_, Propose(_))
        .WillOnce(Invoke([partition, fsId](const braft::Task &task) {
            ASSERT_EQ(partition->DeleteInode(fsId, ROOTINODEID),
                      MetaStatusCode::OK);
            LOG(INFO) << "Partition DeleteInode, fsId = " << fsId
                      << ", inodeId = " << ROOTINODEID;
            task.done->Run();
        }))
        .WillOnce(Invoke([partition, fsId, inode1](const braft::Task &task) {
            ASSERT_EQ(partition->DeleteInode(fsId, inode1.inodeid()),
                      MetaStatusCode::OK);
            LOG(INFO) << "Partition DeleteInode, fsId = " << fsId
                      << ", inodeId = " << inode1.inodeid();
            task.done->Run();
        }))
        .WillOnce(Invoke([partition](const braft::Task &task) {
            LOG(INFO) << "Partition deletePartition";
            task.done->Run();
        }));

    EXPECT_CALL(*s3Adaptor_, Delete(_)).WillOnce(Return(0));
    EXPECT_CALL(*s3Adaptor_, GetS3ClientAdaptorOption(_));
    EXPECT_CALL(*s3Adaptor_, Reinit(_, _, _, _, _));

    manager->Add(partitionId, partitionCleaner, copyset_);

    sleep(4);
    manager->Fini();
    ASSERT_EQ(manager->GetCleanerCount(), 0);
}

TEST_F(PartitionCleanManagerTest, GetFsInfoFail) {
    PartitionInfo partition;
    PartitionCleaner cleaner(
        std::make_shared<Partition>(partition, kvStorage_));
    cleaner.SetMdsClient(mdsCli_);
    cleaner.SetS3Aapter(s3Adaptor_);
    cleaner.SetCopysetNode(copyset_);
    Inode inode;
    inode.set_type(FsFileType::TYPE_S3);
    inode.set_fsid(10);
    EXPECT_CALL(*mdsCli_, GetFsInfo(Matcher<uint32_t>(_), _))
        .WillOnce(Return(FSStatusCode::NOT_FOUND));
    ASSERT_EQ(cleaner.CleanDataAndDeleteInode(inode),
              MetaStatusCode::S3_DELETE_ERR);

    inode.set_fsid(11);
    EXPECT_CALL(*mdsCli_, GetFsInfo(Matcher<uint32_t>(_), _))
        .WillOnce(Return(FSStatusCode::UNKNOWN_ERROR));
    ASSERT_EQ(cleaner.CleanDataAndDeleteInode(inode),
              MetaStatusCode::S3_DELETE_ERR);
}

}  // namespace metaserver
}  // namespace curvefs
