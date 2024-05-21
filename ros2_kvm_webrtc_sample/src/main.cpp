#include <atomic>
#include <charconv>
#include <iostream>
#include <queue>
#include <string_view>
#include <Samples.h>
extern "C" {
#include <x264.h>
}


#include <geometry_msgs/msg/twist.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>

// simplelogger::Logger* logger =
//     simplelogger::LoggerFactory::CreateConsoleLogger();
extern PSampleConfiguration gSampleConfiguration;

struct EncodedFrame {
  std::vector<std::vector<uint8_t>> packets;
  int width;
  int height;
};

template <class T>
class ThreadSafeQueue {
 public:
  ThreadSafeQueue(void) : queue_(), mutex_(), condition_() {}
  ~ThreadSafeQueue(void) {}
  void Enqueue(T&& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(value));
    if (queue_.size() > 10) {
      std::cerr << "EncodedFrame queue got full. abandoned" << std::endl;
      queue_.pop();
    }
    condition_.notify_one();
  }
  T Dequeue(void) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (queue_.empty()) {
      condition_.wait(lock);
    }
    T val = queue_.front();
    queue_.pop();
    return val;
  }

 private:
  std::queue<T> queue_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
};

class KVSClient {
 public:
  static KVSClient& GetInstance() {
    if (!instance) {
      instance = new KVSClient;
    }
    return *instance;
  }
  bool isReady() const { return ready.load(); }
  void QueueEncodedFrame(EncodedFrame&& frame) {
    frameQueue.Enqueue(std::move(frame));
  }
  void RegisterDataChannelMessageCallback(
      std::function<void(std::string_view)>&& callback) {
    dataChannelOnMessageCallback = std::move(callback);
  }
  bool Init(PCHAR channel_name) {
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR pChannelName;
    signalingClientMetrics.version = SIGNALING_CLIENT_METRICS_CURRENT_VERSION;

    SET_INSTRUMENTED_ALLOCATORS();

    // do trickleIce by default
    printf("[KVS Master] Using trickleICE by default\n");

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_ERR((pChannelName = getenv(IOT_CORE_THING_NAME)) != NULL,
            STATUS_INVALID_OPERATION, "AWS_IOT_CORE_THING_NAME must be set");
#else
    pChannelName = channel_name;
#endif

    retStatus = createSampleConfiguration(pChannelName,
                                          SIGNALING_CHANNEL_ROLE_TYPE_MASTER,
                                          TRUE, TRUE, &pSampleConfiguration);
    if (retStatus != STATUS_SUCCESS) {
      printf(
          "[KVS Master] createSampleConfiguration(): operation returned status "
          "code: 0x%08x \n",
          retStatus);
      goto CleanUp;
    }

    printf("[KVS Master] Created signaling channel %s\n", pChannelName);

    if (pSampleConfiguration->enableFileLogging) {
      retStatus = createFileLogger(
          FILE_LOGGING_BUFFER_SIZE, MAX_NUMBER_OF_LOG_FILES,
          (PCHAR)FILE_LOGGER_LOG_FILE_DIRECTORY_PATH, TRUE, TRUE, NULL);
      if (retStatus != STATUS_SUCCESS) {
        printf(
            "[KVS Master] createFileLogger(): operation returned status code: "
            "0x%08x \n",
            retStatus);
        pSampleConfiguration->enableFileLogging = FALSE;
      }
    }

    // Set the audio and video handlers
    // pSampleConfiguration->audioSource = sendAudioPackets;
    pSampleConfiguration->videoSource = KVSClient::sendVideoPackets;
    // pSampleConfiguration->receiveAudioVideoSource = sampleReceiveVideoFrame;
    pSampleConfiguration->onDataChannel = KVSClient::onDataChannel;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    printf("[KVS Master] Finished setting audio and video handlers\n");

    // Initialize KVS WebRTC. This must be done before anything else, and must
    // only be done once.
    retStatus = initKvsWebRtc();
    if (retStatus != STATUS_SUCCESS) {
      printf(
          "[KVS Master] initKvsWebRtc(): operation returned status code: "
          "0x%08x "
          "\n",
          retStatus);
      goto CleanUp;
    }
    printf("[KVS Master] KVS WebRTC initialization completed successfully\n");

    pSampleConfiguration->signalingClientCallbacks.messageReceivedFn =
        signalingMessageReceived;

    strcpy(pSampleConfiguration->clientInfo.clientId, SAMPLE_MASTER_CLIENT_ID);

    retStatus = createSignalingClientSync(
        &pSampleConfiguration->clientInfo, &pSampleConfiguration->channelInfo,
        &pSampleConfiguration->signalingClientCallbacks,
        pSampleConfiguration->pCredentialProvider,
        &pSampleConfiguration->signalingClientHandle);
    if (retStatus != STATUS_SUCCESS) {
      printf(
          "[KVS Master] createSignalingClientSync(): operation returned status "
          "code: 0x%08x \n",
          retStatus);
      goto CleanUp;
    }
    printf("[KVS Master] Signaling client created successfully\n");

    // Enable the processing of the messages
    retStatus =
        signalingClientFetchSync(pSampleConfiguration->signalingClientHandle);
    if (retStatus != STATUS_SUCCESS) {
      printf(
          "[KVS Master] signalingClientFetchSync(): operation returned status "
          "code: 0x%08x \n",
          retStatus);
      goto CleanUp;
    }

    retStatus =
        signalingClientConnectSync(pSampleConfiguration->signalingClientHandle);
    if (retStatus != STATUS_SUCCESS) {
      printf(
          "[KVS Master] signalingClientConnectSync(): operation returned "
          "status "
          "code: 0x%08x \n",
          retStatus);
      goto CleanUp;
    }
    printf("[KVS Master] Signaling client connection to socket established\n");

    gSampleConfiguration = pSampleConfiguration;

    printf("[KVS Master] Channel %s set up done \n", pChannelName);
    return true;
  CleanUp:
    return false;
  }
  void Run() {
    cleanupThread = std::thread([this] {
      STATUS retStatus = STATUS_SUCCESS;
      retStatus = sessionCleanupWait(pSampleConfiguration);
      if (retStatus != STATUS_SUCCESS) {
        printf(
            "[KVS Master] sessionCleanupWait(): operation returned status "
            "code: 0x%08x \n",
            retStatus);
      }
      printf("[KVS Master] Streaming session terminated\n");
    });
  }
  void CleanUp() {
    printf("[KVS Master] Cleaning up....\n");
    if (cleanupThread.joinable()) {
      ATOMIC_STORE_BOOL(&pSampleConfiguration->interrupted, TRUE);
      cleanupThread.join();
    }
    STATUS retStatus = STATUS_SUCCESS;
    if (pSampleConfiguration != NULL) {
      // Kick of the termination sequence
      ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

      if (IS_VALID_MUTEX_VALUE(
              pSampleConfiguration->sampleConfigurationObjLock)) {
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
      }

      // Cancel the media thread
      if (pSampleConfiguration->mediaThreadStarted) {
        DLOGD("Canceling media thread");
        THREAD_CANCEL(pSampleConfiguration->mediaSenderTid);
      }

      if (IS_VALID_MUTEX_VALUE(
              pSampleConfiguration->sampleConfigurationObjLock)) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
      }

      if (pSampleConfiguration->mediaSenderTid != INVALID_TID_VALUE) {
        THREAD_JOIN(pSampleConfiguration->mediaSenderTid, NULL);
      }

      if (pSampleConfiguration->enableFileLogging) {
        freeFileLogger();
      }
      retStatus = signalingClientGetMetrics(
          pSampleConfiguration->signalingClientHandle, &signalingClientMetrics);
      if (retStatus == STATUS_SUCCESS) {
        logSignalingClientStats(&signalingClientMetrics);
      } else {
        printf(
            "[KVS Master] signalingClientGetMetrics() operation returned "
            "status code: 0x%08x\n",
            retStatus);
      }
      retStatus =
          freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
      if (retStatus != STATUS_SUCCESS) {
        printf(
            "[KVS Master] freeSignalingClient(): operation returned status "
            "code: 0x%08x",
            retStatus);
      }

      retStatus = freeSampleConfiguration(&pSampleConfiguration);
      if (retStatus != STATUS_SUCCESS) {
        printf(
            "[KVS Master] freeSampleConfiguration(): operation returned status "
            "code: 0x%08x",
            retStatus);
      }
    }
    printf("[KVS Master] Cleanup done\n");

    RESET_INSTRUMENTED_ALLOCATORS();

    delete instance;
    instance = nullptr;
  }
  static VOID onDataChannelMessage(UINT64 customData,
                                   PRtcDataChannel pDataChannel, BOOL isBinary,
                                   PBYTE pMessage, UINT32 pMessageLen) {
    UNUSED_PARAM(customData);
    UNUSED_PARAM(pDataChannel);
    if (isBinary) {
      DLOGI("DataChannel Binary Message");
    } else {
      DLOGI("DataChannel String Message: %.*s\n", pMessageLen, pMessage);
    }
    KVSClient& kvs_client = GetInstance();
    kvs_client.dataChannelOnMessageCallback(
        std::string_view{reinterpret_cast<char*>(pMessage), pMessageLen});
  }
  static VOID onDataChannel(UINT64 customData,
                            PRtcDataChannel pRtcDataChannel) {
    DLOGI("New DataChannel has been opened %s \n", pRtcDataChannel->name);
    dataChannelOnMessage(pRtcDataChannel, customData, onDataChannelMessage);
  }
  static PVOID sendVideoPackets(PVOID args) {
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration)args;
    RtcEncoderStats encoderStats;
    Frame frame;
    UINT32 frameSize;
    STATUS status;
    UINT32 i;
    KVSClient& kvs_client = GetInstance();
    MEMSET(&encoderStats, 0x00, SIZEOF(RtcEncoderStats));

    if (pSampleConfiguration == NULL) {
      printf(
          "[KVS Master] sendVideoPackets(): operation returned status code: "
          "0x%08x \n",
          STATUS_NULL_ARG);
      goto CleanUp;
    }

    frame.presentationTs = 0;

    kvs_client.ready.store(true);
    printf("[KVS Master] sendVideoPackets(): started\n");
    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
      const EncodedFrame encoded_frame = kvs_client.frameQueue.Dequeue();
      for (const auto& packet : encoded_frame.packets) {
        frameSize = packet.size();
        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->videoBufferSize) {
          pSampleConfiguration->pVideoFrameBuffer = (PBYTE)MEMREALLOC(
              pSampleConfiguration->pVideoFrameBuffer, frameSize);
          if (pSampleConfiguration->pVideoFrameBuffer == NULL) {
            printf(
                "[KVS Master] Video frame Buffer reallocation failed...%s "
                "(code "
                "%d)\n",
                strerror(errno), errno);
            printf(
                "[KVS Master] MEMREALLOC(): operation returned status code: "
                "0x%08x "
                "\n",
                STATUS_NOT_ENOUGH_MEMORY);
            goto CleanUp;
          }

          pSampleConfiguration->videoBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pVideoFrameBuffer;
        frame.size = frameSize;
        memcpy(frame.frameData, packet.data(), packet.size());

        encoderStats.width = encoded_frame.width;
        encoderStats.height = encoded_frame.height;
        encoderStats.targetBitrate = 262000;
        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;

        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
          status =
              writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]
                             ->pVideoRtcRtpTransceiver,
                         &frame);
          encoderStats.encodeTimeMsec =
              4;  // update encode time to an arbitrary
                  // number to demonstrate stats update
          updateEncoderStats(pSampleConfiguration->sampleStreamingSessionList[i]
                                 ->pVideoRtcRtpTransceiver,
                             &encoderStats);
          if (status != STATUS_SRTP_NOT_READY_YET) {
            if (status != STATUS_SUCCESS) {
#ifdef VERBOSE
              printf("writeFrame() failed with 0x%08x\n", status);
#endif
            }
          }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
      }
    }
  CleanUp:
    CHK_LOG_ERR(retStatus);
    return (PVOID)(ULONG_PTR)retStatus;
  }

 private:
  PSampleConfiguration pSampleConfiguration = NULL;
  SignalingClientMetrics signalingClientMetrics;
  ThreadSafeQueue<EncodedFrame> frameQueue;
  std::atomic<bool> ready{false};
  std::thread cleanupThread;
  std::function<void(std::string_view)> dataChannelOnMessageCallback;

  static KVSClient* instance;
};

KVSClient* KVSClient::instance = nullptr;


class x264Encoder {
public:
    void Init(int nWidth = 1920, int nHeight = 1080, int fps = 30) {
        width = nWidth;
        height = nHeight;

        // Set default parameters
        x264_param_default_preset(&param, "veryfast", "zerolatency");

        // Modify parameters as needed
        param.i_bitdepth = 8;
        param.i_width = nWidth;
        param.i_height = nHeight;
        param.i_fps_num = fps;
        param.i_fps_den = 1;
        param.i_keyint_max = fps;
        param.b_intra_refresh = 1;
        param.rc.i_rc_method = X264_RC_CRF;
        param.rc.f_rf_constant = 25;
        param.rc.f_rf_constant_max = 35;
        param.i_sps_id = 7;
        param.b_repeat_headers = 1;
        param.i_log_level = X264_LOG_INFO;

        // Apply profile
        if (x264_param_apply_profile(&param, "high") < 0) {
            std::cerr << "Failed to apply x264 profile" << std::endl;
            exit(EXIT_FAILURE);
        }

        // Open encoder
        encoder = x264_encoder_open(&param);
        if (!encoder) {
            std::cerr << "Failed to open x264 encoder" << std::endl;
            exit(EXIT_FAILURE);
        }

        // Allocate image buffer
        x264_picture_alloc(&pic_in, param.i_csp, param.i_width, param.i_height);
    }

    EncodedFrame Encode(int nWidth, int nHeight, const std::vector<uint8_t>& image) {
        width = nWidth;
        height = nHeight;
        // printf("width : %f, height : %f", width, height)

        // Convert RGB to YUV420
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int index = y * width + x;
                int r = image[3 * index + 0];
                int g = image[3 * index + 1];
                int b = image[3 * index + 2];

                pic_in.img.plane[0][y * pic_in.img.i_stride[0] + x] = (uint8_t)(0.257 * r + 0.504 * g + 0.098 * b + 16);
                if (x % 2 == 0 && y % 2 == 0) {
                    pic_in.img.plane[1][(y / 2) * pic_in.img.i_stride[1] + (x / 2)] = (uint8_t)(-0.148 * r - 0.291 * g + 0.439 * b + 128);
                    pic_in.img.plane[2][(y / 2) * pic_in.img.i_stride[2] + (x / 2)] = (uint8_t)(0.439 * r - 0.368 * g - 0.071 * b + 128);
                }
            }
        }

        x264_nal_t* nals;
        int i_nal;
        x264_picture_t pic_out;
        int frame_size = x264_encoder_encode(encoder, &nals, &i_nal, &pic_in, &pic_out);

        EncodedFrame encoded_frame;
        encoded_frame.width = width;
        encoded_frame.height = height;

        if (frame_size > 0) {
            for (int i = 0; i < i_nal; i++) {
                std::vector<uint8_t> packet(nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
                encoded_frame.packets.push_back(packet);
            }
        }

        return encoded_frame;
    }

    ~x264Encoder() {
        x264_picture_clean(&pic_in);
        if (encoder) {
            x264_encoder_close(encoder);
        }
    }

private:
    x264_param_t param;
    x264_t* encoder = nullptr;
    x264_picture_t pic_in;
    int width;
    int height;
};


class ROS2KVSWebRtcProxy : public rclcpp::Node {
public:
    ROS2KVSWebRtcProxy(KVSClient& kvs_client,
                       const rclcpp::NodeOptions& options = rclcpp::NodeOptions())
        : ROS2KVSWebRtcProxy(kvs_client, "", options) {}

    ROS2KVSWebRtcProxy(KVSClient& kvs_client, const std::string& name_space,
                       const rclcpp::NodeOptions& options = rclcpp::NodeOptions())
        : Node("ros2_kvs_webrtc_proxy", name_space, options),
          kvs_client_{kvs_client} {
        subscription_ = this->create_subscription<sensor_msgs::msg::Image>(
            "/camera/image_raw", rclcpp::QoS(10),
            std::bind(&ROS2KVSWebRtcProxy::ImageCallback, this,
                      std::placeholders::_1));
        encoder_.Init();
    }

    void ImageCallback(const sensor_msgs::msg::Image::SharedPtr msg) {
      if (!kvs_client_.isReady()) {
        return;
      }
      std::cout<<"width : "<<msg->width<<", height : "<<msg->height<<std::endl;
      EncodedFrame encoded_frame = encoder_.Encode(msg->width, msg->height, msg->data);
      kvs_client_.QueueEncodedFrame(std::move(encoded_frame));
    }

private:
    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr subscription_;
    x264Encoder encoder_;  // Use the x264Encoder instead of CudaEncoder
    KVSClient& kvs_client_;
    bool encoder_initialized_ = false;
};

int main(int argc, char* argv[]) {
  KVSClient& kvs_client = KVSClient::GetInstance();
  if (kvs_client.Init(SAMPLE_CHANNEL_NAME)) {
    kvs_client.Run();
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<ROS2KVSWebRtcProxy>(kvs_client));
    rclcpp::shutdown();
  }
  kvs_client.CleanUp();
  return 0;
}
