static ResourceTracker* sResourceTracker = nullptr;
static uint32_t sFeatureBits = 0;
static constexpr uint32_t kWatchdogBufferMax = 1'000;

class VkEncoder::Impl {
   public:
    Impl(gfxstream::guest::IOStream* stream) : m_stream(stream), m_logEncodes(false) {
        if (!sResourceTracker) sResourceTracker = ResourceTracker::get();
        m_stream.incStreamRef();
        const char* emuVkLogEncodesPropName = "qemu.vk.log";
        char encodeProp[PROPERTY_VALUE_MAX];
        if (property_get(emuVkLogEncodesPropName, encodeProp, nullptr) > 0) {
            m_logEncodes = atoi(encodeProp) > 0;
        }
        sFeatureBits = m_stream.getFeatureBits();
    }

    ~Impl() { m_stream.decStreamRef(); }

    VulkanCountingStream* countingStream() { return &m_countingStream; }
    VulkanStreamGuest* stream() { return &m_stream; }
    BumpPool* pool() { return &m_pool; }
    ResourceTracker* resources() { return ResourceTracker::get(); }
    Validation* validation() { return &m_validation; }

    void log(const char* text) {
        if (!m_logEncodes) return;
        ALOGD("encoder log: %s", text);
    }

    void flush() {
        lock();
        m_stream.flush();
        unlock();
    }

    // can be recursive
    void lock() {
        while (mLock.test_and_set(std::memory_order_acquire))
            ;
    }

    void unlock() { mLock.clear(std::memory_order_release); }

   private:
    VulkanCountingStream m_countingStream;
    VulkanStreamGuest m_stream;
    BumpPool m_pool;

    Validation m_validation;
    bool m_logEncodes;
    std::atomic_flag mLock = ATOMIC_FLAG_INIT;
};

VkEncoder::~VkEncoder() {}

struct EncoderAutoLock {
    EncoderAutoLock(VkEncoder* enc) : mEnc(enc) { mEnc->lock(); }
    ~EncoderAutoLock() { mEnc->unlock(); }
    VkEncoder* mEnc;
};

VkEncoder::VkEncoder(gfxstream::guest::IOStream* stream,
                     gfxstream::guest::HealthMonitor<>* healthMonitor)
    : mImpl(new VkEncoder::Impl(stream)), mHealthMonitor(healthMonitor) {}

void VkEncoder::flush() { mImpl->flush(); }

void VkEncoder::lock() { mImpl->lock(); }

void VkEncoder::unlock() { mImpl->unlock(); }

void VkEncoder::incRef() { __atomic_add_fetch(&refCount, 1, __ATOMIC_SEQ_CST); }

bool VkEncoder::decRef() {
    if (0 == __atomic_sub_fetch(&refCount, 1, __ATOMIC_SEQ_CST)) {
        delete this;
        return true;
    }
    return false;
}

std::string VkEncoder::getPacketContents(const uint8_t* ptr, size_t len) {
  std::string result;
  std::unique_ptr<char[]> buf(new char[3]);
  for (size_t i = 0; i < len; i++) {
    std::snprintf(buf.get(), 3, "%02X", ptr[i]);
    result += " " + std::string(buf.get(), buf.get() + 2);
  }
  return result;
}
