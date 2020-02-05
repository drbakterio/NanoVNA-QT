#include "firmwareupdater.H"
#include <xavna/platform_abstraction.H>
#include <errno.h>
#include <string.h>
#include <unistd.h>

// this dependency can be removed if you provide implementations
// of qToLittleEndian() and qFromLittleEndian()
#include <qendian.h>

static int writeAll(int fd, const void* buf, int len) {
    const uint8_t* buf1 = static_cast<const uint8_t*> (buf);
    int off = 0;
    while(off<len) {
        auto r = write(fd, buf1 + off, size_t(len - off));
        if(r < 0) return int(r);
        if(r == 0) {
            errno = EPIPE;
            return -1;
        }
        off += r;
    }
    return off;
}
static int checkError(int r) {
    if(r < 0) throw runtime_error(strerror(errno));
    return r;
}

FirmwareUpdater::FirmwareUpdater() {

}

FirmwareUpdater::~FirmwareUpdater() {
    close();
}

void FirmwareUpdater::open(const char* dev) {
    ttyFD = checkError(xavna_open_serial(dev));
    char buf[64] = {};
    write(ttyFD, buf, sizeof(buf));

    xavna_drainfd(ttyFD);
    usleep(10000);
    xavna_drainfd(ttyFD);

    if(readRegister(0xf3) != 0xff) {
        close();
        throw logic_error("not in DFU mode");
    }
}

void FirmwareUpdater::close() {
    if(ttyFD < 0) return;
    ::close(ttyFD);
    ttyFD = -1;
}

void FirmwareUpdater::beginUploadFirmware(uint32_t dstAddr,
                                          const function<int(uint8_t* buf, int len)>& reader,
                                          const function<void(int progress)>& cb) {
    // set flash write address
    checkError(writeRegister32(0xe0, dstAddr));

    this->_cb = cb;
    this->_reader = reader;
    checkError(pthread_create(&_pth, nullptr, &_flashThread, this));
}

exception* FirmwareUpdater::endUploadFirmware() {
    if(!_reader)
        throw logic_error("endUploadFirmware called without a prior beginUploadFirmware");
    _reader = nullptr;
    void* ret;
    checkError(pthread_join(_pth, &ret));
    return static_cast<exception*>(ret);
}

void FirmwareUpdater::setUserArgument(uint32_t arg) {
    checkError(writeRegister32(0xe8, arg));
}

void FirmwareUpdater::reboot() {
    checkError(writeRegister(0xef, 0x5e));
}

int FirmwareUpdater::readRegister(uint8_t addr) {
    uint8_t buf[] = {
        0x10, addr
    };
    if(writeAll(ttyFD,buf,sizeof(buf)) != int(sizeof(buf)))
        return -1;

    uint8_t rBuf[1];
    if(read(ttyFD, rBuf, 1) != 1)
        return -1;
    return rBuf[0];
}

int FirmwareUpdater::writeRegister(uint8_t addr, uint8_t val) {
    uint8_t buf[] = {
        0x20, addr, val
    };
    return (writeAll(ttyFD,buf,sizeof(buf)) == int(sizeof(buf))) ? 0 : -1;
}

int FirmwareUpdater::writeRegister32(uint8_t addr, uint32_t val) {
    uint8_t buf[] = {
        0x22, addr, 0,0,0,0
    };
    qToLittleEndian(val, buf + 2);
    return (writeAll(ttyFD,buf,sizeof(buf)) == int(sizeof(buf))) ? 0 : -1;
}

int FirmwareUpdater::_sendBytes(const uint8_t* data, int len) {
    if(len > 255)
        throw out_of_range("FirmwareUpdater::_sendBytes can not send > 255 bytes");

    // cmd: write FIFO 0xe4
    string cmd = "\x28\xe4";
    cmd += char(uint8_t(len));
    cmd.append(reinterpret_cast<const char*>(data), size_t(len));
    // cmd: echo version
    cmd += 0x0d;
    int totalLen = int(cmd.length());
    if(writeAll(ttyFD, cmd.data(), totalLen) != totalLen)
        return -1;
    return 0;
}

int FirmwareUpdater::_waitSend() {
    uint8_t buf;
    auto tmp = read(ttyFD, &buf, 1);
    if(tmp == 0)
        errno = EPIPE;
    if(tmp <= 0)
        return -1;
    return 0;
}

void* FirmwareUpdater::_flashThread(void* v) {
    FirmwareUpdater* t = static_cast<FirmwareUpdater*>(v);
    return t->flashThread();
}


static inline timespec currTime() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}
static int64_t timediff(const timespec& start, const timespec& stop) {
    int64_t tmp = stop.tv_nsec - start.tv_nsec;
    tmp += int64_t(stop.tv_sec - start.tv_sec) * 1000000000LL;
    return tmp;
}
void* FirmwareUpdater::flashThread() {
    constexpr int bufSize = 255;
    uint8_t buf[bufSize];
    int outstanding = 0;
    int progress = 0;
    auto lastNotify = currTime();

    while(true) {
        auto br = _reader(buf, bufSize);
        if(br < 0)
            goto fail;
        if(br == 0)
            break;

        int res = _sendBytes(buf, int(br));
        if(res < 0)
            goto fail;

        progress += br;
        auto t = currTime();
        if(timediff(lastNotify, t) > 100000000) {
            lastNotify = t;
            _cb(progress);
        }

        outstanding++;
        if(outstanding > 5) {
            if(_waitSend() < 0)
                goto fail;
            outstanding--;
        }
    }
    while(outstanding > 0) {
        if(_waitSend() < 0)
            goto fail;
        outstanding--;
    }
    _cb(-1);
    return nullptr;
fail:
    auto ex = new runtime_error(strerror(errno));
    _cb(-1);
    return ex;
}