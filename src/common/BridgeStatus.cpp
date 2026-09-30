#include "common/BridgeStatus.h"

#include "common/BridgeConfig.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>

namespace dgmod {

namespace {

std::wstring MappingName() { return std::format(L"{}.v{}", kBridgeStatusMappingPrefix, kBridgeStatusVersion); }

}  // namespace

uint64_t FileTimeNow() {
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    return (uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime;
}

Result<BridgeStatusMapping> BridgeStatusMapping::CreateForWriter() {
    BridgeStatusMapping m;
    m.mapping_.Reset(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(BridgeStatusBlock),
                                          MappingName().c_str()));
    if (!m.mapping_) return FailWin32(L"CreateFileMapping");
    m.view_.Reset(::MapViewOfFile(m.mapping_.Get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BridgeStatusBlock)));
    if (!m.view_) return FailWin32(L"MapViewOfFile");
    BridgeStatusBlock* b = m.Block();
    b->version = kBridgeStatusVersion;
    std::atomic_ref<uint32_t>(b->magic).store(kBridgeStatusMagic, std::memory_order_release);
    return m;
}

Result<BridgeStatusMapping> BridgeStatusMapping::OpenForReader() {
    BridgeStatusMapping m;
    m.mapping_.Reset(::OpenFileMappingW(FILE_MAP_READ, FALSE, MappingName().c_str()));
    if (!m.mapping_) return FailWin32(L"OpenFileMapping");
    m.view_.Reset(::MapViewOfFile(m.mapping_.Get(), FILE_MAP_READ, 0, 0, sizeof(BridgeStatusBlock)));
    if (!m.view_) return FailWin32(L"MapViewOfFile");
    if (m.Block()->magic != kBridgeStatusMagic || m.Block()->version != kBridgeStatusVersion)
        return Fail(E_UNEXPECTED, L"Bridge status version mismatch");
    return m;
}

void BridgeStatusMapping::Publish(const BridgeStatusData& d) {
    if (BridgeStatusBlock* b = Block()) SeqWrite(b->seq, b->data, d);
}

bool BridgeStatusMapping::Read(BridgeStatusData& out) const {
    const BridgeStatusBlock* b = Block();
    return b && SeqRead(b->seq, b->data, out);
}

void BridgeStatusMapping::PushMeter(const BridgeMeterEntry& e) {
    BridgeStatusBlock* b = Block();
    if (!b) return;
    std::atomic_ref<uint64_t> written(b->meter.written);
    const uint64_t n = written.load(std::memory_order_relaxed);
    std::memcpy(static_cast<void*>(&b->meter.entries[n % kBridgeMeterEntries]), &e, sizeof(e));
    written.store(n + 1, std::memory_order_release);
}

void BridgeStatusMapping::ReadMeter(uint64_t& cursor, std::vector<BridgeMeterEntry>& out) const {
    const BridgeStatusBlock* b = Block();
    if (!b) return;
    std::atomic_ref<uint64_t> written(const_cast<uint64_t&>(b->meter.written));
    const uint64_t end = written.load(std::memory_order_acquire);
    if (end < cursor) cursor = 0;  // a new mapping counts from zero again
    uint64_t begin = std::max(cursor, end > kBridgeMeterEntries ? end - kBridgeMeterEntries : 0);
    const size_t first = out.size();
    for (uint64_t i = begin; i < end; ++i) {
        BridgeMeterEntry e;
        std::memcpy(&e, &b->meter.entries[i % kBridgeMeterEntries], sizeof(e));
        out.push_back(e);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    // The writer may have reused the oldest slots while they were copied (entry `after` is possibly half written).
    const uint64_t after = written.load(std::memory_order_relaxed);
    if (after + 1 > begin + kBridgeMeterEntries) {
        const uint64_t torn = std::min<uint64_t>(after + 1 - kBridgeMeterEntries - begin, end - begin);
        out.erase(out.begin() + static_cast<ptrdiff_t>(first),
                  out.begin() + static_cast<ptrdiff_t>(first + torn));
    }
    cursor = end;
}

}  // namespace dgmod
