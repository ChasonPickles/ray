#include "ray/gcs/redis_gcs_client.h"

#include <unistd.h>
#include "ray/common/ray_config.h"
#include "ray/gcs/redis_actor_info_accessor.h"
#include "ray/gcs/redis_context.h"
#include "ray/gcs/redis_job_info_accessor.h"

static void GetRedisShards(redisContext *context, std::vector<std::string> &addresses,
                           std::vector<int> &ports) {
  // Get the total number of Redis shards in the system.
  int num_attempts = 0;
  redisReply *reply = nullptr;
  while (num_attempts < RayConfig::instance().redis_db_connect_retries()) {
    // Try to read the number of Redis shards from the primary shard. If the
    // entry is present, exit.
    reply = reinterpret_cast<redisReply *>(redisCommand(context, "GET NumRedisShards"));
    if (reply->type != REDIS_REPLY_NIL) {
      break;
    }

    // Sleep for a little, and try again if the entry isn't there yet. */
    freeReplyObject(reply);
    usleep(RayConfig::instance().redis_db_connect_wait_milliseconds() * 1000);
    num_attempts++;
  }
  RAY_CHECK(num_attempts < RayConfig::instance().redis_db_connect_retries())
      << "No entry found for NumRedisShards";
  RAY_CHECK(reply->type == REDIS_REPLY_STRING)
      << "Expected string, found Redis type " << reply->type << " for NumRedisShards";
  int num_redis_shards = atoi(reply->str);
  RAY_CHECK(num_redis_shards >= 1) << "Expected at least one Redis shard, "
                                   << "found " << num_redis_shards;
  freeReplyObject(reply);

  // Get the addresses of all of the Redis shards.
  num_attempts = 0;
  while (num_attempts < RayConfig::instance().redis_db_connect_retries()) {
    // Try to read the Redis shard locations from the primary shard. If we find
    // that all of them are present, exit.
    reply =
        reinterpret_cast<redisReply *>(redisCommand(context, "LRANGE RedisShards 0 -1"));
    if (static_cast<int>(reply->elements) == num_redis_shards) {
      break;
    }

    // Sleep for a little, and try again if not all Redis shard addresses have
    // been added yet.
    freeReplyObject(reply);
    usleep(RayConfig::instance().redis_db_connect_wait_milliseconds() * 1000);
    num_attempts++;
  }
  RAY_CHECK(num_attempts < RayConfig::instance().redis_db_connect_retries())
      << "Expected " << num_redis_shards << " Redis shard addresses, found "
      << reply->elements;

  // Parse the Redis shard addresses.
  for (size_t i = 0; i < reply->elements; ++i) {
    // Parse the shard addresses and ports.
    RAY_CHECK(reply->element[i]->type == REDIS_REPLY_STRING);
    std::string addr;
    std::stringstream ss(reply->element[i]->str);
    getline(ss, addr, ':');
    addresses.push_back(addr);
    int port;
    ss >> port;
    ports.push_back(port);
  }
  freeReplyObject(reply);
}

namespace ray {

namespace gcs {

RedisGcsClient::RedisGcsClient(const GcsClientOptions &options) : GcsClient(options) {
#if RAY_USE_NEW_GCS
  command_type_ = CommandType::kChain;
#else
  command_type_ = CommandType::kRegular;
#endif
    const auto &heartbeat_batch_added =
            [this](gcs::RedisGcsClient *client, const ClientID &id,
                   const HeartbeatBatchTableData &heartbeat_batch) {
                HeartbeatBatchAdded(heartbeat_batch);
            };
    //TODO error handling if subscribe unsuccessful
    (void) heartbeat_batch_table_->Subscribe(
            JobID::Nil(), ClientID::Nil(), heartbeat_batch_added,
            /*subscribe_callback=*/nullptr,
            /*done_callback=*/nullptr);

}

RedisGcsClient::RedisGcsClient(const GcsClientOptions &options, CommandType command_type)
    : GcsClient(options), command_type_(command_type) {

const auto &heartbeat_batch_added =
        [this](gcs::RedisGcsClient *client, const ClientID &id,
               const HeartbeatBatchTableData &heartbeat_batch) {
            HeartbeatBatchAdded(heartbeat_batch);
        };
    //TODO error handling if subscribe unsuccessful

    (void) heartbeat_batch_table_->Subscribe(
        JobID::Nil(), ClientID::Nil(), heartbeat_batch_added,
        /*subscribe_callback=*/nullptr,
        /*done_callback=*/nullptr);


}

Status RedisGcsClient::Connect(boost::asio::io_service &io_service) {
  RAY_CHECK(!is_connected_);

  if (options_.server_ip_.empty()) {
    RAY_LOG(ERROR) << "Failed to connect, gcs service address is empty.";
    return Status::Invalid("gcs service address is invalid!");
  }

  primary_context_ = std::make_shared<RedisContext>(io_service);

  RAY_CHECK_OK(primary_context_->Connect(options_.server_ip_, options_.server_port_,
                                         /*sharding=*/true,
                                         /*password=*/options_.password_));

  if (!options_.is_test_client_) {
    // Moving sharding into constructor defaultly means that sharding = true.
    // This design decision may worth a look.
    std::vector<std::string> addresses;
    std::vector<int> ports;
    GetRedisShards(primary_context_->sync_context(), addresses, ports);
    if (addresses.empty()) {
      RAY_CHECK(ports.empty());
      addresses.push_back(options_.server_ip_);
      ports.push_back(options_.server_port_);
    }

    for (size_t i = 0; i < addresses.size(); ++i) {
      // Populate shard_contexts.
      shard_contexts_.push_back(std::make_shared<RedisContext>(io_service));
      RAY_CHECK_OK(shard_contexts_[i]->Connect(addresses[i], ports[i], /*sharding=*/true,
                                               /*password=*/options_.password_));
    }
  } else {
    shard_contexts_.push_back(std::make_shared<RedisContext>(io_service));
    RAY_CHECK_OK(shard_contexts_[0]->Connect(options_.server_ip_, options_.server_port_,
                                             /*sharding=*/true,
                                             /*password=*/options_.password_));
  }

  Attach(io_service);

  actor_table_.reset(new ActorTable({primary_context_}, this));
  direct_actor_table_.reset(new DirectActorTable({primary_context_}, this));

  // TODO(micafan) Modify ClientTable' Constructor(remove ClientID) in future.
  // We will use NodeID instead of ClientID.
  // For worker/driver, it might not have this field(NodeID).
  // For raylet, NodeID should be initialized in raylet layer(not here).
  client_table_.reset(new ClientTable({primary_context_}, this, ClientID::FromRandom()));

  error_table_.reset(new ErrorTable({primary_context_}, this));
  job_table_.reset(new JobTable({primary_context_}, this));
  heartbeat_batch_table_.reset(new HeartbeatBatchTable({primary_context_}, this));
  // Tables below would be sharded.
  object_table_.reset(new ObjectTable(shard_contexts_, this));
  raylet_task_table_.reset(new raylet::TaskTable(shard_contexts_, this, command_type_));
  task_reconstruction_log_.reset(new TaskReconstructionLog(shard_contexts_, this));
  task_lease_table_.reset(new TaskLeaseTable(shard_contexts_, this));
  heartbeat_table_.reset(new HeartbeatTable(shard_contexts_, this));
  profile_table_.reset(new ProfileTable(shard_contexts_, this));
  actor_checkpoint_table_.reset(new ActorCheckpointTable(shard_contexts_, this));
  actor_checkpoint_id_table_.reset(new ActorCheckpointIdTable(shard_contexts_, this));
  resource_table_.reset(new DynamicResourceTable({primary_context_}, this));

  actor_accessor_.reset(new RedisActorInfoAccessor(this));
  job_accessor_.reset(new RedisJobInfoAccessor(this));

  is_connected_ = true;

  RAY_LOG(INFO) << "RedisGcsClient Connected.";

  return Status::OK();
}

void RedisGcsClient::Disconnect() {
  RAY_CHECK(is_connected_);
  is_connected_ = false;
  RAY_LOG(INFO) << "RedisGcsClient Disconnected.";
  // TODO(micafan): Synchronously unregister node if this client is Raylet.
}

void RedisGcsClient::Attach(boost::asio::io_service &io_service) {
  // Take care of sharding contexts.
  RAY_CHECK(shard_asio_async_clients_.empty()) << "Attach shall be called only once";
  for (std::shared_ptr<RedisContext> context : shard_contexts_) {
    shard_asio_async_clients_.emplace_back(
        new RedisAsioClient(io_service, context->async_context()));
    shard_asio_subscribe_clients_.emplace_back(
        new RedisAsioClient(io_service, context->subscribe_context()));
  }
  asio_async_auxiliary_client_.reset(
      new RedisAsioClient(io_service, primary_context_->async_context()));
  asio_subscribe_auxiliary_client_.reset(
      new RedisAsioClient(io_service, primary_context_->subscribe_context()));
}

std::string RedisGcsClient::DebugString() const {
  std::stringstream result;
  result << "RedisGcsClient:";
  result << "\n- TaskTable: " << raylet_task_table_->DebugString();
  result << "\n- ActorTable: " << actor_table_->DebugString();
  result << "\n- TaskReconstructionLog: " << task_reconstruction_log_->DebugString();
  result << "\n- TaskLeaseTable: " << task_lease_table_->DebugString();
  result << "\n- HeartbeatTable: " << heartbeat_table_->DebugString();
  result << "\n- ErrorTable: " << error_table_->DebugString();
  result << "\n- ProfileTable: " << profile_table_->DebugString();
  result << "\n- ClientTable: " << client_table_->DebugString();
  result << "\n- JobTable: " << job_table_->DebugString();
  return result.str();
}

ObjectTable &RedisGcsClient::object_table() { return *object_table_; }

raylet::TaskTable &RedisGcsClient::raylet_task_table() { return *raylet_task_table_; }

ActorTable &RedisGcsClient::actor_table() { return *actor_table_; }

DirectActorTable &RedisGcsClient::direct_actor_table() { return *direct_actor_table_; }

TaskReconstructionLog &RedisGcsClient::task_reconstruction_log() {
  return *task_reconstruction_log_;
}

TaskLeaseTable &RedisGcsClient::task_lease_table() { return *task_lease_table_; }

ClientTable &RedisGcsClient::client_table() { return *client_table_; }

HeartbeatTable &RedisGcsClient::heartbeat_table() { return *heartbeat_table_; }

HeartbeatBatchTable &RedisGcsClient::heartbeat_batch_table() {
  return *heartbeat_batch_table_;
}

ErrorTable &RedisGcsClient::error_table() { return *error_table_; }

JobTable &RedisGcsClient::job_table() { return *job_table_; }

ProfileTable &RedisGcsClient::profile_table() { return *profile_table_; }

ActorCheckpointTable &RedisGcsClient::actor_checkpoint_table() {
  return *actor_checkpoint_table_;
}

ActorCheckpointIdTable &RedisGcsClient::actor_checkpoint_id_table() {
  return *actor_checkpoint_id_table_;
}

DynamicResourceTable &RedisGcsClient::resource_table() { return *resource_table_; }

void SubmitTask(TaskSpecification task) {
    RAY_LOG(DEBUG) << task.DebugString() << " stupid" ;
}

void RedisGcsClient::HeartbeatAdded(const ClientID &client_id,
                                 const HeartbeatTableData &heartbeat_data) {

    ResourceSet remote_total(VectorFromProtobuf(heartbeat_data.resources_total_label()),
                             VectorFromProtobuf(heartbeat_data.resources_total_capacity()));
    ResourceSet remote_available(
            VectorFromProtobuf(heartbeat_data.resources_available_label()),
            VectorFromProtobuf(heartbeat_data.resources_available_capacity()));
    ResourceSet remote_load(VectorFromProtobuf(heartbeat_data.resource_load_label()),
                            VectorFromProtobuf(heartbeat_data.resource_load_capacity()));

    // Locate the client id in remote client table and update available resources based on
    // the received heartbeat information.
    auto it = cluster_resource_map_.find(client_id);
    if (it == cluster_resource_map_.end()) {
        // Haven't received the client registration for this client yet, skip this heartbeat.
        RAY_LOG(INFO) << "[HeartbeatAdded]: received heartbeat from unknown client id "
                      << client_id;
        cluster_resource_map_[client_id] = SchedulingResources();

        SchedulingResources &remote_resources = cluster_resource_map_[client_id];
        remote_resources.SetAvailableResources(std::move(remote_available));
        remote_resources.SetLoadResources(std::move(remote_load));
    }
    else{
        SchedulingResources &remote_resources = it->second;
        remote_resources.SetAvailableResources(std::move(remote_available));
        remote_resources.SetLoadResources(std::move(remote_load));
    }
}


void RedisGcsClient::HeartbeatBatchAdded(const HeartbeatBatchTableData &heartbeat_batch) {
    // Update load information provided by each heartbeat.
    // TODO(edoakes): this isn't currently used, but will be used to refresh the LRU
    // cache in the object store.
    std::unordered_set<ObjectID> active_object_ids;
    for (const auto &heartbeat_data : heartbeat_batch.batch()) {
        for (int i = 0; i < heartbeat_data.active_object_id_size(); i++) {
            active_object_ids.insert(ObjectID::FromBinary(heartbeat_data.active_object_id(i)));
        }
        const ClientID &client_id = ClientID::FromBinary(heartbeat_data.client_id());
        HeartbeatAdded(client_id, heartbeat_data);
    }

    RAY_LOG(DEBUG) << "Total active object IDs received: " << active_object_ids.size();
}

}  // namespace gcs

}  // namespace ray
