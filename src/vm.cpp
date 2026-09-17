#include "concept/vm.hpp"

#include <xorstr.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif
#endif

namespace cpt {
namespace {

struct Frame {
    std::uint64_t id{};
    std::size_t return_ip{};
    std::size_t stack_base{};
    std::vector<std::uint64_t> locals;
    std::uint64_t constructor_result{};
    std::uint8_t complexity{};
    // Locals of an obfuscated frame are XOR-masked in memory. A slot is pinned
    // (unmasked in place) once its address is taken, so pointer and native
    // access always see plaintext storage. Empty until the frame first masks.
    std::vector<bool> local_pinned{};
};

struct PointerTarget {
    enum class Kind { local, field, native_address, heap_block }
        kind{Kind::local};
    std::uint64_t owner{};
    std::uint64_t index{};
    ValueType native_type{ValueType::u64};
};

struct HeapBlock {
    std::vector<std::uint8_t> bytes;
    bool dynamic{};
    bool freed{};
    std::uint64_t frame_id{};
};

struct HeapPointerKey {
    std::uint64_t owner{};
    std::uint64_t index{};
    ValueType type{ValueType::u64};

    bool operator==(const HeapPointerKey&) const = default;
};

struct HeapPointerKeyHash {
    std::size_t operator()(const HeapPointerKey& value) const noexcept {
        auto mixed = value.owner ^ std::rotl(value.index, 23);
        mixed ^= (static_cast<std::uint64_t>(value.type) + 1) << 56;
        mixed ^= mixed >> 30;
        mixed *= 0xbf58476d1ce4e5b9ULL;
        mixed ^= mixed >> 27;
        return static_cast<std::size_t>(mixed);
    }
};

std::uint64_t mix_handler_state(std::uint64_t value) noexcept {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint8_t handler_variant(const std::uint64_t seed,
                             const Op op) noexcept {
    const auto domain = static_cast<std::uint64_t>(op) + 1;
    return static_cast<std::uint8_t>(
        mix_handler_state(seed ^ (domain * 0x9e3779b97f4a7c15ULL)) & 3);
}

// Per-site selector for mixed boolean-arithmetic handler variants. Unlike the
// operand-order/substitution variant this is derived from the instruction
// offset as well, so two identical operations at different addresses evaluate
// through different (but arithmetically identical) expression trees.
std::uint8_t handler_mba(const std::uint64_t seed, const Op op,
                         const std::size_t instruction_offset) noexcept {
    const auto domain = static_cast<std::uint64_t>(op) + 1;
    return static_cast<std::uint8_t>(
        mix_handler_state(
            seed ^ (domain * 0x9e3779b97f4a7c15ULL) ^
            (static_cast<std::uint64_t>(instruction_offset) *
             0xff51afd7ed558ccdULL)) &
        0xff);
}

#if defined(_MSC_VER)
#define CONCEPT_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define CONCEPT_NOINLINE __attribute__((noinline))
#else
#define CONCEPT_NOINLINE
#endif

CONCEPT_NOINLINE void execute_handler_junk(
    const std::uint64_t seed, const Op op,
    const std::size_t instruction_offset, const std::uint8_t variant) {
    volatile std::uint64_t sink = mix_handler_state(
        seed ^ static_cast<std::uint64_t>(instruction_offset) ^
        ((static_cast<std::uint64_t>(op) + 1) << 48));
    auto value = static_cast<std::uint64_t>(sink);
    switch (variant) {
    case 0:
        value += 0xd6e8feb86659fd93ULL;
        value = std::rotl(value, 17);
        value ^= value >> 23;
        break;
    case 1:
        value ^= std::rotl(value, 29);
        value *= 0xa24baed4963ee407ULL;
        value += 0x9fb21c651e98df25ULL;
        break;
    case 2:
        value = std::rotr(value ^ 0xc2b2ae3d27d4eb4fULL, 11);
        value -= value << 7;
        value ^= 0x165667b19e3779f9ULL;
        break;
    default:
        value = ~value;
        value += std::rotl(value, 37);
        value ^= value >> 31;
        break;
    }
    sink = value;
}

CONCEPT_NOINLINE std::uint64_t
handler_value_barrier(const std::uint64_t value) {
    volatile std::uint64_t sink = value;
    return sink;
}

#undef CONCEPT_NOINLINE

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
using SocketLength = int;

template <std::size_t Size>
std::string decode_import_name(
    const std::array<std::uint8_t, Size>& encoded) {
    std::string decoded(Size, '\0');
    for (std::size_t index = 0; index < Size; ++index) {
        const auto key = static_cast<std::uint8_t>(0xa7 + index * 0x3d);
        decoded[index] = static_cast<char>(encoded[index] ^ key);
    }
    return decoded;
}

class WinsockApi {
public:
    decltype(&::socket) socket_function{};
    decltype(&::connect) connect_function{};
    decltype(&::bind) bind_function{};
    decltype(&::listen) listen_function{};
    decltype(&::accept) accept_function{};
    decltype(&::send) send_function{};
    decltype(&::recv) receive_function{};
    decltype(&::closesocket) close_function{};
    decltype(&::getaddrinfo) get_address_function{};
    decltype(&::freeaddrinfo) free_address_function{};

    WinsockApi() {
        static constexpr std::array<std::uint8_t, 10> module_name{
            0xd0, 0x97, 0x13, 0x01, 0xa8, 0xea, 0x3b, 0x36, 0xe3, 0xa0};
        module_ = LoadLibraryA(decode_import_name(module_name).c_str());
        if (module_ == nullptr) {
            throw std::runtime_error(xorstr_(
                "Concept VM could not load its socket component"));
        }

        try {
            load_functions();
        } catch (...) {
            FreeLibrary(module_);
            module_ = nullptr;
            throw;
        }
    }

    ~WinsockApi() {
        if (started_) {
            cleanup_function_();
        }
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    WinsockApi(const WinsockApi&) = delete;
    WinsockApi& operator=(const WinsockApi&) = delete;

private:
    HMODULE module_{};
    decltype(&::WSACleanup) cleanup_function_{};
    bool started_{};

    template <typename Function, std::size_t Size>
    Function resolve(const std::array<std::uint8_t, Size>& encoded_name) {
        const auto name = decode_import_name(encoded_name);
        const auto address = GetProcAddress(module_, name.c_str());
        if (address == nullptr) {
            throw std::runtime_error(xorstr_(
                "Concept VM could not resolve a socket operation"));
        }
        static_assert(sizeof(Function) == sizeof(address));
        return std::bit_cast<Function>(address);
    }

    void load_functions() {
        static constexpr std::array<std::uint8_t, 10> startup_name{
            0xf0, 0xb7, 0x60, 0x0d, 0xef, 0xb9, 0x67, 0x26, 0xfa, 0xbc};
        static constexpr std::array<std::uint8_t, 10> cleanup_name{
            0xf0, 0xb7, 0x60, 0x1d, 0xf7, 0xbd, 0x74, 0x3c, 0xfa, 0xbc};
        static constexpr std::array<std::uint8_t, 6> socket_name{
            0xd4, 0x8b, 0x42, 0x35, 0xfe, 0xac};
        static constexpr std::array<std::uint8_t, 7> connect_name{
            0xc4, 0x8b, 0x4f, 0x30, 0xfe, 0xbb, 0x61};
        static constexpr std::array<std::uint8_t, 4> bind_name{
            0xc5, 0x8d, 0x4f, 0x3a};
        static constexpr std::array<std::uint8_t, 6> listen_name{
            0xcb, 0x8d, 0x52, 0x2a, 0xfe, 0xb6};
        static constexpr std::array<std::uint8_t, 6> accept_name{
            0xc6, 0x87, 0x42, 0x3b, 0xeb, 0xac};
        static constexpr std::array<std::uint8_t, 4> send_name{
            0xd4, 0x81, 0x4f, 0x3a};
        static constexpr std::array<std::uint8_t, 4> receive_name{
            0xd5, 0x81, 0x42, 0x28};
        static constexpr std::array<std::uint8_t, 11> close_name{
            0xc4, 0x88, 0x4e, 0x2d, 0xfe, 0xab, 0x7a, 0x31, 0xe4, 0xa9,
            0x7d};
        static constexpr std::array<std::uint8_t, 11> get_address_name{
            0xc0, 0x81, 0x55, 0x3f, 0xff, 0xbc, 0x67, 0x3b, 0xe1, 0xaa,
            0x66};
        static constexpr std::array<std::uint8_t, 12> free_address_name{
            0xc1, 0x96, 0x44, 0x3b, 0xfa, 0xbc, 0x71, 0x20, 0xe6, 0xa2,
            0x6f, 0x29};

        const auto startup =
            resolve<decltype(&::WSAStartup)>(startup_name);
        cleanup_function_ =
            resolve<decltype(cleanup_function_)>(cleanup_name);
        socket_function = resolve<decltype(socket_function)>(socket_name);
        connect_function = resolve<decltype(connect_function)>(connect_name);
        bind_function = resolve<decltype(bind_function)>(bind_name);
        listen_function = resolve<decltype(listen_function)>(listen_name);
        accept_function = resolve<decltype(accept_function)>(accept_name);
        send_function = resolve<decltype(send_function)>(send_name);
        receive_function = resolve<decltype(receive_function)>(receive_name);
        close_function = resolve<decltype(close_function)>(close_name);
        get_address_function =
            resolve<decltype(get_address_function)>(get_address_name);
        free_address_function =
            resolve<decltype(free_address_function)>(free_address_name);

        WSADATA data{};
        if (startup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error(
                xorstr_("Concept VM could not initialize Winsock"));
        }
        started_ = true;
    }
};

class NativeMemoryApi {
public:
    decltype(&::ReadProcessMemory) read_function{};
    decltype(&::WriteProcessMemory) write_function{};

    NativeMemoryApi() {
        static constexpr std::array<std::uint8_t, 12> module_name{
            0xcc, 0x81, 0x53, 0x30, 0xfe, 0xb4, 0x26, 0x60, 0xa1, 0xa8,
            0x65, 0x2a};
        module_ = LoadLibraryA(decode_import_name(module_name).c_str());
        if (module_ == nullptr) {
            throw std::runtime_error(xorstr_(
                "Concept VM could not load native-memory support"));
        }

        static constexpr std::array<std::uint8_t, 17> read_name{
            0xf5, 0x81, 0x40, 0x3a, 0xcb, 0xaa, 0x7a, 0x31, 0xea,
            0xbf, 0x7a, 0x0b, 0xe6, 0xad, 0x92, 0x48, 0x0e};
        static constexpr std::array<std::uint8_t, 18> write_name{
            0xf0, 0x96, 0x48, 0x2a, 0xfe, 0x88, 0x67, 0x3d, 0xec,
            0xa9, 0x7a, 0x35, 0xce, 0xa5, 0x90, 0x55, 0x05, 0xcd};
        read_function = resolve<decltype(read_function)>(read_name);
        write_function = resolve<decltype(write_function)>(write_name);
    }

    ~NativeMemoryApi() {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    NativeMemoryApi(const NativeMemoryApi&) = delete;
    NativeMemoryApi& operator=(const NativeMemoryApi&) = delete;

private:
    HMODULE module_{};

    template <typename Function, std::size_t Size>
    Function resolve(const std::array<std::uint8_t, Size>& encoded_name) {
        const auto name = decode_import_name(encoded_name);
        const auto address = GetProcAddress(module_, name.c_str());
        if (address == nullptr) {
            throw std::runtime_error(xorstr_(
                "Concept VM could not resolve native-memory support"));
        }
        static_assert(sizeof(Function) == sizeof(address));
        return std::bit_cast<Function>(address);
    }
};

class CertificateApi {
public:
    decltype(&::CertCreateCertificateContext) create_context{};
    decltype(&::CertFreeCertificateContext) free_context{};
    decltype(&::CertOpenStore) open_store{};
    decltype(&::CertCloseStore) close_store{};
    decltype(&::CertAddCertificateContextToStore) add_context{};
    decltype(&::CertGetCertificateChain) get_chain{};
    decltype(&::CertFreeCertificateChain) free_chain{};
    decltype(&::CertVerifyCertificateChainPolicy) verify_policy{};

    CertificateApi() {
        module_ = LoadLibraryA(xorstr_("crypt32.dll"));
        if (module_ == nullptr) {
            throw std::runtime_error(xorstr_(
                "Concept VM could not load system certificate support"));
        }
        try {
            create_context = resolve<decltype(create_context)>(
                xorstr_("CertCreateCertificateContext"));
            free_context = resolve<decltype(free_context)>(
                xorstr_("CertFreeCertificateContext"));
            open_store = resolve<decltype(open_store)>(
                xorstr_("CertOpenStore"));
            close_store = resolve<decltype(close_store)>(
                xorstr_("CertCloseStore"));
            add_context = resolve<decltype(add_context)>(
                xorstr_("CertAddCertificateContextToStore"));
            get_chain = resolve<decltype(get_chain)>(
                xorstr_("CertGetCertificateChain"));
            free_chain = resolve<decltype(free_chain)>(
                xorstr_("CertFreeCertificateChain"));
            verify_policy = resolve<decltype(verify_policy)>(
                xorstr_("CertVerifyCertificateChainPolicy"));
        } catch (...) {
            FreeLibrary(module_);
            module_ = nullptr;
            throw;
        }
    }

    ~CertificateApi() {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    CertificateApi(const CertificateApi&) = delete;
    CertificateApi& operator=(const CertificateApi&) = delete;

private:
    HMODULE module_{};

    template <typename Function>
    Function resolve(const char* name) {
        const auto address = GetProcAddress(module_, name);
        if (address == nullptr) {
            throw std::runtime_error(xorstr_(
                "Concept VM could not resolve system certificate support"));
        }
        static_assert(sizeof(Function) == sizeof(address));
        return std::bit_cast<Function>(address);
    }
};

WinsockApi& winsock_api() {
    static WinsockApi api;
    return api;
}

NativeMemoryApi& native_memory_api() {
    static NativeMemoryApi api;
    return api;
}

CertificateApi& certificate_api() {
    static CertificateApi api;
    return api;
}

bool close_native_socket(const NativeSocket handle) {
    return winsock_api().close_function(handle) == 0;
}

NativeSocket platform_socket(const int family, const int type,
                             const int protocol) {
    return winsock_api().socket_function(family, type, protocol);
}

int platform_connect(const NativeSocket handle, const sockaddr* address,
                     const SocketLength length) {
    return winsock_api().connect_function(handle, address, length);
}

int platform_bind(const NativeSocket handle, const sockaddr* address,
                  const SocketLength length) {
    return winsock_api().bind_function(handle, address, length);
}

int platform_listen(const NativeSocket handle, const int backlog) {
    return winsock_api().listen_function(handle, backlog);
}

NativeSocket platform_accept(const NativeSocket handle) {
    return winsock_api().accept_function(handle, nullptr, nullptr);
}

int platform_send(const NativeSocket handle, const char* data,
                  const int size, const int flags) {
    return winsock_api().send_function(handle, data, size, flags);
}

int platform_receive(const NativeSocket handle, char* data, const int size,
                     const int flags) {
    return winsock_api().receive_function(handle, data, size, flags);
}

int platform_get_address(const char* host, const char* service,
                         const addrinfo* hints, addrinfo** addresses) {
    return winsock_api().get_address_function(host, service, hints, addresses);
}

void platform_free_address(addrinfo* addresses) {
    winsock_api().free_address_function(addresses);
}

constexpr int send_flags = 0;
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
using SocketLength = socklen_t;

bool close_native_socket(const NativeSocket handle) {
    return ::close(handle) == 0;
}

NativeSocket platform_socket(const int family, const int type,
                             const int protocol) {
    return ::socket(family, type, protocol);
}

int platform_connect(const NativeSocket handle, const sockaddr* address,
                     const SocketLength length) {
    return ::connect(handle, address, length);
}

int platform_bind(const NativeSocket handle, const sockaddr* address,
                  const SocketLength length) {
    return ::bind(handle, address, length);
}

int platform_listen(const NativeSocket handle, const int backlog) {
    return ::listen(handle, backlog);
}

NativeSocket platform_accept(const NativeSocket handle) {
    return ::accept(handle, nullptr, nullptr);
}

int platform_send(const NativeSocket handle, const char* data,
                  const int size, const int flags) {
    return static_cast<int>(::send(handle, data, static_cast<std::size_t>(size),
                                   flags));
}

int platform_receive(const NativeSocket handle, char* data, const int size,
                     const int flags) {
    return static_cast<int>(::recv(handle, data,
                                   static_cast<std::size_t>(size), flags));
}

int platform_get_address(const char* host, const char* service,
                         const addrinfo* hints, addrinfo** addresses) {
    return ::getaddrinfo(host, service, hints, addresses);
}

void platform_free_address(addrinfo* addresses) {
    ::freeaddrinfo(addresses);
}

#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif
#endif

bool verify_system_x509_chain(
    const std::string& host, const std::span<const std::uint8_t> encoded) {
#ifdef _WIN32
    if (host.empty() || encoded.empty()) {
        return false;
    }

    try {
        auto& api = certificate_api();
        std::vector<PCCERT_CONTEXT> certificates;
        std::size_t offset = 0;
        while (offset < encoded.size()) {
            if (encoded.size() - offset < 4) {
                for (const auto certificate : certificates) {
                    api.free_context(certificate);
                }
                return false;
            }
            const auto length =
                (static_cast<std::uint32_t>(encoded[offset]) << 24) |
                (static_cast<std::uint32_t>(encoded[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(encoded[offset + 2]) << 8) |
                static_cast<std::uint32_t>(encoded[offset + 3]);
            offset += 4;
            if (length == 0 || length > encoded.size() - offset) {
                for (const auto certificate : certificates) {
                    api.free_context(certificate);
                }
                return false;
            }
            const auto certificate = api.create_context(
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                encoded.data() + offset, length);
            if (certificate == nullptr) {
                for (const auto existing : certificates) {
                    api.free_context(existing);
                }
                return false;
            }
            certificates.push_back(certificate);
            offset += length;
        }
        if (certificates.empty()) {
            return false;
        }

        const auto store = api.open_store(
            CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
        if (store == nullptr) {
            for (const auto certificate : certificates) {
                api.free_context(certificate);
            }
            return false;
        }
        bool intermediates_added = true;
        for (std::size_t index = 1; index < certificates.size(); ++index) {
            if (api.add_context(store, certificates[index],
                                CERT_STORE_ADD_ALWAYS, nullptr) == FALSE) {
                intermediates_added = false;
                break;
            }
        }

        PCCERT_CHAIN_CONTEXT chain = nullptr;
        CERT_CHAIN_PARA chain_parameters{};
        chain_parameters.cbSize = sizeof(chain_parameters);
        bool valid = intermediates_added &&
                     api.get_chain(nullptr, certificates.front(), nullptr,
                                   store, &chain_parameters, 0, nullptr,
                                   &chain) != FALSE;
        if (valid) {
            std::wstring server_name(host.begin(), host.end());
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_parameters{};
            ssl_parameters.cbSize = sizeof(ssl_parameters);
            ssl_parameters.dwAuthType = AUTHTYPE_SERVER;
            ssl_parameters.pwszServerName = server_name.data();
            CERT_CHAIN_POLICY_PARA policy_parameters{};
            policy_parameters.cbSize = sizeof(policy_parameters);
            policy_parameters.pvExtraPolicyPara = &ssl_parameters;
            CERT_CHAIN_POLICY_STATUS policy_status{};
            policy_status.cbSize = sizeof(policy_status);
            valid = api.verify_policy(
                        CERT_CHAIN_POLICY_SSL, chain, &policy_parameters,
                        &policy_status) != FALSE &&
                    policy_status.dwError == S_OK;
        }

        if (chain != nullptr) {
            api.free_chain(chain);
        }
        api.close_store(store, 0);
        for (const auto certificate : certificates) {
            api.free_context(certificate);
        }
        return valid;
    } catch (...) {
        return false;
    }
#elif defined(__APPLE__)
    if (host.empty() || encoded.empty()) {
        return false;
    }

    // The payload is a sequence of big-endian u32 length prefixes, each
    // followed by a DER-encoded certificate; the first entry is the leaf.
    const auto release = [](CFTypeRef reference) {
        if (reference != nullptr) {
            CFRelease(reference);
        }
    };

    CFMutableArrayRef certificates =
        CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (certificates == nullptr) {
        return false;
    }

    bool parsed = true;
    std::size_t offset = 0;
    while (offset < encoded.size()) {
        if (encoded.size() - offset < 4) {
            parsed = false;
            break;
        }
        const auto length =
            (static_cast<std::uint32_t>(encoded[offset]) << 24) |
            (static_cast<std::uint32_t>(encoded[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(encoded[offset + 2]) << 8) |
            static_cast<std::uint32_t>(encoded[offset + 3]);
        offset += 4;
        if (length == 0 || length > encoded.size() - offset) {
            parsed = false;
            break;
        }
        CFDataRef data = CFDataCreate(
            kCFAllocatorDefault, encoded.data() + offset,
            static_cast<CFIndex>(length));
        if (data == nullptr) {
            parsed = false;
            break;
        }
        SecCertificateRef certificate =
            SecCertificateCreateWithData(kCFAllocatorDefault, data);
        release(data);
        if (certificate == nullptr) {
            parsed = false;
            break;
        }
        CFArrayAppendValue(certificates, certificate);
        release(certificate);
        offset += length;
    }

    if (!parsed || CFArrayGetCount(certificates) == 0) {
        release(certificates);
        return false;
    }

    CFStringRef server_name = CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const std::uint8_t*>(host.data()),
        static_cast<CFIndex>(host.size()), kCFStringEncodingUTF8, false);
    SecPolicyRef policy =
        server_name != nullptr ? SecPolicyCreateSSL(true, server_name)
                               : nullptr;
    release(server_name);
    if (policy == nullptr) {
        release(certificates);
        return false;
    }

    SecTrustRef trust = nullptr;
    const OSStatus status =
        SecTrustCreateWithCertificates(certificates, policy, &trust);
    release(policy);
    release(certificates);
    if (status != errSecSuccess || trust == nullptr) {
        release(trust);
        return false;
    }

    const bool valid = SecTrustEvaluateWithError(trust, nullptr);
    release(trust);
    return valid;
#else
    static_cast<void>(host);
    static_cast<void>(encoded);
    return false;
#endif
}

#ifdef _WIN64
std::uint64_t invoke_native_call(
    const std::string& module_name, const std::string& symbol_name,
    const std::array<std::uintptr_t, 4>& arguments,
    const std::size_t argument_count) {
    const auto module = LoadLibraryA(module_name.c_str());
    if (module == nullptr) {
        throw std::runtime_error(
            xorstr_("Concept VM could not load a native module"));
    }
    const auto address = GetProcAddress(module, symbol_name.c_str());
    if (address == nullptr) {
        FreeLibrary(module);
        throw std::runtime_error(
            xorstr_("Concept VM could not resolve a native symbol"));
    }

    using Function0 = std::uintptr_t(WINAPI*)();
    using Function1 = std::uintptr_t(WINAPI*)(std::uintptr_t);
    using Function2 =
        std::uintptr_t(WINAPI*)(std::uintptr_t, std::uintptr_t);
    using Function3 = std::uintptr_t(WINAPI*)(
        std::uintptr_t, std::uintptr_t, std::uintptr_t);
    using Function4 = std::uintptr_t(WINAPI*)(
        std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

    std::uintptr_t result = 0;
    switch (argument_count) {
    case 0:
        result = std::bit_cast<Function0>(address)();
        break;
    case 1:
        result = std::bit_cast<Function1>(address)(arguments[0]);
        break;
    case 2:
        result = std::bit_cast<Function2>(address)(arguments[0], arguments[1]);
        break;
    case 3:
        result = std::bit_cast<Function3>(address)(
            arguments[0], arguments[1], arguments[2]);
        break;
    case 4:
        result = std::bit_cast<Function4>(address)(
            arguments[0], arguments[1], arguments[2], arguments[3]);
        break;
    default:
        FreeLibrary(module);
        throw std::runtime_error(
            xorstr_("Concept native call has too many arguments"));
    }
    FreeLibrary(module);
    return static_cast<std::uint64_t>(result);
}
#else
std::uint64_t invoke_native_call(
    const std::string&, const std::string&,
    const std::array<std::uintptr_t, 4>&, const std::size_t) {
    throw std::runtime_error(
        xorstr_("Concept native calls require Windows x64"));
}
#endif

std::size_t native_value_size(const ValueType type) {
    switch (type) {
    case ValueType::boolean:
    case ValueType::i8:
    case ValueType::u8:
        return 1;
    case ValueType::i16:
    case ValueType::u16:
        return 2;
    case ValueType::i32:
    case ValueType::u32:
    case ValueType::f32:
        return 4;
    case ValueType::i64:
    case ValueType::u64:
    case ValueType::f64:
        return 8;
    case ValueType::text:
        throw std::runtime_error(
            xorstr_("Concept VM cannot access native string memory"));
    case ValueType::void_type:
        throw std::runtime_error(
            xorstr_("Concept VM cannot dereference a void pointer"));
    }
    throw std::logic_error(xorstr_("invalid native pointer value type"));
}

std::size_t heap_value_size(const ValueType type) {
    return type == ValueType::text ? sizeof(std::uint64_t)
                                   : native_value_size(type);
}

bool read_native_value(const std::uint64_t address, const ValueType type,
                       std::uint64_t& value) {
    value = 0;
#ifdef _WIN32
    SIZE_T transferred = 0;
    const auto size = native_value_size(type);
    const auto current_process =
        reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1));
    return native_memory_api().read_function(
               current_process,
               reinterpret_cast<const void*>(
                   static_cast<std::uintptr_t>(address)),
               &value, size, &transferred) != FALSE &&
           transferred == size;
#else
    static_cast<void>(address);
    static_cast<void>(type);
    return false;
#endif
}

bool write_native_value(const std::uint64_t address, const ValueType type,
                        const std::uint64_t value) {
#ifdef _WIN32
    SIZE_T transferred = 0;
    const auto size = native_value_size(type);
    const auto current_process =
        reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1));
    return native_memory_api().write_function(
               current_process,
               reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)),
               &value, size, &transferred) != FALSE &&
           transferred == size;
#else
    static_cast<void>(address);
    static_cast<void>(type);
    static_cast<void>(value);
    return false;
#endif
}

std::uint64_t socket_bits(const NativeSocket handle) {
    if (handle == invalid_socket) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(handle);
}

NativeSocket native_socket(const std::uint64_t bits) {
    if (bits == std::numeric_limits<std::uint64_t>::max()) {
        return invalid_socket;
    }
    return static_cast<NativeSocket>(bits);
}

NativeSocket open_tcp_socket() {
    return platform_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

bool connect_tcp_socket(const NativeSocket handle, const std::string& host,
                        const std::uint16_t port) {
    if (handle == invalid_socket || host.empty()) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    if (platform_get_address(host.c_str(), service.c_str(), &hints,
                             &addresses) != 0) {
        return false;
    }

    bool connected = false;
    for (auto* address = addresses; address != nullptr;
         address = address->ai_next) {
        if (platform_connect(
                handle, address->ai_addr,
                static_cast<SocketLength>(address->ai_addrlen)) == 0) {
            connected = true;
            break;
        }
    }
    platform_free_address(addresses);
    return connected;
}

bool bind_tcp_socket(const NativeSocket handle, const std::string& host,
                     const std::uint16_t port) {
    if (handle == invalid_socket) {
        return false;
    }

    const bool any_address = host.empty() || host == xorstr_("*");
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = any_address ? AI_PASSIVE : 0;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    if (platform_get_address(any_address ? nullptr : host.c_str(),
                             service.c_str(), &hints, &addresses) != 0) {
        return false;
    }

    bool bound = false;
    for (auto* address = addresses; address != nullptr;
         address = address->ai_next) {
        if (platform_bind(
                handle, address->ai_addr,
                static_cast<SocketLength>(address->ai_addrlen)) == 0) {
            bound = true;
            break;
        }
    }
    platform_free_address(addresses);
    return bound;
}

bool listen_tcp_socket(const NativeSocket handle, const int backlog) {
    return handle != invalid_socket && platform_listen(handle, backlog) == 0;
}

NativeSocket accept_tcp_socket(const NativeSocket handle) {
    if (handle == invalid_socket) {
        return invalid_socket;
    }
    return platform_accept(handle);
}

std::int64_t send_tcp_text(const NativeSocket handle,
                           const std::string& text) {
    if (handle == invalid_socket) {
        return -1;
    }

    std::size_t sent = 0;
    while (sent < text.size()) {
        const auto remaining = text.size() - sent;
        const auto chunk = static_cast<int>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(
                           std::numeric_limits<int>::max())));
        const auto result =
            platform_send(handle, text.data() + sent, chunk, send_flags);
        if (result <= 0) {
            return sent == 0 ? -1 : static_cast<std::int64_t>(sent);
        }
        sent += static_cast<std::size_t>(result);
    }
    return static_cast<std::int64_t>(sent);
}

std::int64_t send_tcp_bytes(
    const NativeSocket handle, const std::span<const std::uint8_t> bytes) {
    if (handle == invalid_socket) {
        return -1;
    }

    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto remaining = bytes.size() - sent;
        const auto chunk = static_cast<int>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const auto result = platform_send(
            handle, reinterpret_cast<const char*>(bytes.data() + sent), chunk,
            send_flags);
        if (result <= 0) {
            return sent == 0 ? -1 : static_cast<std::int64_t>(sent);
        }
        sent += static_cast<std::size_t>(result);
    }
    return static_cast<std::int64_t>(sent);
}

std::int64_t receive_tcp_bytes(const NativeSocket handle,
                               const std::span<std::uint8_t> bytes) {
    if (handle == invalid_socket) {
        return -1;
    }
    if (bytes.empty()) {
        return 0;
    }
    const auto count = static_cast<int>(std::min<std::size_t>(
        bytes.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    return platform_receive(handle, reinterpret_cast<char*>(bytes.data()),
                            count, 0);
}

std::string receive_tcp_text(const NativeSocket handle) {
    if (handle == invalid_socket) {
        return {};
    }

    std::array<char, 4096> buffer{};
    const auto received = platform_receive(
        handle, buffer.data(), static_cast<int>(buffer.size()), 0);
    return received <= 0
               ? std::string{}
               : std::string(buffer.data(), static_cast<std::size_t>(received));
}

class OperandReader {
public:
    OperandReader(const std::vector<std::uint8_t>& code,
                  const std::size_t opcode_offset,
                  const std::uint32_t region_begin,
                  const std::uint32_t region_end,
                  const bool reversed)
        : code_(code),
          cursor_(static_cast<std::ptrdiff_t>(opcode_offset) +
                  (reversed ? -1 : 1)),
          begin_(region_begin),
          end_(region_end),
          step_(reversed ? -1 : 1) {}

    [[nodiscard]] std::uint8_t read_u8() {
        if (cursor_ < begin_ || cursor_ >= end_) {
            throw std::runtime_error(
                xorstr_("VM operand read crossed its bytecode region"));
        }
        const auto value = code_[static_cast<std::size_t>(cursor_)];
        cursor_ += step_;
        return value;
    }

    [[nodiscard]] std::uint16_t read_u16() {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(read_u8()) |
            static_cast<std::uint16_t>(read_u8()) << 8);
    }

    [[nodiscard]] std::uint32_t read_u32() {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(read_u8()) << (index * 8);
        }
        return value;
    }

    [[nodiscard]] std::uint64_t read_u64() {
        std::uint64_t value = 0;
        for (unsigned index = 0; index < 8; ++index) {
            value |= static_cast<std::uint64_t>(read_u8()) << (index * 8);
        }
        return value;
    }

    [[nodiscard]] ValueType read_type() {
        const auto raw_type = read_u8();
        if (raw_type > static_cast<std::uint8_t>(ValueType::void_type)) {
            throw std::runtime_error(
                xorstr_("VM encountered an invalid value type"));
        }
        return static_cast<ValueType>(raw_type);
    }

private:
    const std::vector<std::uint8_t>& code_;
    std::ptrdiff_t cursor_;
    std::ptrdiff_t begin_;
    std::ptrdiff_t end_;
    std::ptrdiff_t step_;
};

unsigned integral_width(const ValueType type) {
    switch (type) {
    case ValueType::i8:
    case ValueType::u8:
        return 8;
    case ValueType::i16:
    case ValueType::u16:
        return 16;
    case ValueType::i32:
    case ValueType::u32:
        return 32;
    case ValueType::i64:
    case ValueType::u64:
        return 64;
    default:
        throw std::runtime_error(
            xorstr_("VM expected an integral value type"));
    }
}

std::uint64_t bit_mask(const unsigned width) {
    return width == 64 ? std::numeric_limits<std::uint64_t>::max()
                       : (std::uint64_t{1} << width) - 1;
}

std::uint64_t normalize_integral(const ValueType type,
                                 const std::uint64_t bits) {
    return bits & bit_mask(integral_width(type));
}

std::int64_t signed_value(const ValueType type, const std::uint64_t bits) {
    const auto width = integral_width(type);
    auto normalized = normalize_integral(type, bits);
    if (width < 64 &&
        (normalized & (std::uint64_t{1} << (width - 1))) != 0) {
        normalized |= ~bit_mask(width);
    }
    return std::bit_cast<std::int64_t>(normalized);
}

float f32_value(const std::uint64_t bits) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
}

double f64_value(const std::uint64_t bits) {
    return std::bit_cast<double>(bits);
}

std::uint64_t f32_bits(const float value) {
    return std::bit_cast<std::uint32_t>(value);
}

std::uint64_t f64_bits(const double value) {
    return std::bit_cast<std::uint64_t>(value);
}

bool truthy(const ValueType type, const std::uint64_t bits) {
    if (type == ValueType::text || type == ValueType::void_type) {
        throw std::runtime_error(
            xorstr_("string cannot be used as a boolean"));
    }
    if (type == ValueType::boolean) {
        return bits != 0;
    }
    if (type == ValueType::f32) {
        return f32_value(bits) != 0.0F;
    }
    if (type == ValueType::f64) {
        return f64_value(bits) != 0.0;
    }
    return normalize_integral(type, bits) != 0;
}

long double numeric_value(const ValueType type, const std::uint64_t bits) {
    if (type == ValueType::boolean) {
        return bits == 0 ? 0.0L : 1.0L;
    }
    if (type == ValueType::f32) {
        return f32_value(bits);
    }
    if (type == ValueType::f64) {
        return f64_value(bits);
    }
    if (is_signed_integral(type)) {
        return static_cast<long double>(signed_value(type, bits));
    }
    return static_cast<long double>(normalize_integral(type, bits));
}

std::uint64_t floating_to_integral(const long double value,
                                   const ValueType target) {
    if (!std::isfinite(value)) {
        throw std::runtime_error(xorstr_(
            "cannot convert a non-finite floating value to an integer"));
    }

    const auto width = integral_width(target);
    const auto truncated = std::trunc(value);
    if (is_signed_integral(target)) {
        const auto magnitude = std::ldexp(1.0L, static_cast<int>(width - 1));
        if (truncated < -magnitude || truncated >= magnitude) {
            throw std::runtime_error(xorstr_(
                "floating value is outside the target integer range"));
        }
        return normalize_integral(
            target, static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(truncated)));
    }

    const auto upper_bound = std::ldexp(1.0L, static_cast<int>(width));
    if (truncated < 0.0L || truncated >= upper_bound) {
        throw std::runtime_error(xorstr_(
            "floating value is outside the target integer range"));
    }
    return normalize_integral(target,
                              static_cast<std::uint64_t>(truncated));
}

std::uint64_t convert_value(const std::uint64_t bits,
                            const ValueType source,
                            const ValueType target) {
    if (source == ValueType::text || target == ValueType::text ||
        source == ValueType::void_type ||
        target == ValueType::void_type) {
        throw std::runtime_error(xorstr_(
            "string conversion is not supported by the Concept VM"));
    }
    if (target == ValueType::boolean) {
        return truthy(source, bits) ? 1 : 0;
    }
    if (target == ValueType::f32) {
        return f32_bits(static_cast<float>(numeric_value(source, bits)));
    }
    if (target == ValueType::f64) {
        return f64_bits(static_cast<double>(numeric_value(source, bits)));
    }
    if (is_floating(source)) {
        return floating_to_integral(numeric_value(source, bits), target);
    }
    if (source == ValueType::boolean) {
        return normalize_integral(target, bits == 0 ? 0 : 1);
    }
    if (is_signed_integral(source)) {
        return normalize_integral(
            target,
            static_cast<std::uint64_t>(signed_value(source, bits)));
    }
    return normalize_integral(target, normalize_integral(source, bits));
}

std::uint64_t floating_arithmetic(const Op op, const ValueType type,
                                  const std::uint64_t left,
                                  const std::uint64_t right) {
    if (type == ValueType::f32) {
        const auto lhs = f32_value(left);
        const auto rhs = f32_value(right);
        switch (op) {
        case Op::add:
            return f32_bits(lhs + rhs);
        case Op::subtract:
            return f32_bits(lhs - rhs);
        case Op::multiply:
            return f32_bits(lhs * rhs);
        case Op::divide:
            return f32_bits(lhs / rhs);
        default:
            break;
        }
    } else {
        const auto lhs = f64_value(left);
        const auto rhs = f64_value(right);
        switch (op) {
        case Op::add:
            return f64_bits(lhs + rhs);
        case Op::subtract:
            return f64_bits(lhs - rhs);
        case Op::multiply:
            return f64_bits(lhs * rhs);
        case Op::divide:
            return f64_bits(lhs / rhs);
        default:
            break;
        }
    }
    throw std::runtime_error(
        xorstr_("invalid floating-point VM operation"));
}

std::uint64_t integral_arithmetic(const Op op, const ValueType type,
                                  const std::uint64_t left,
                                  const std::uint64_t right,
                                  const std::uint8_t mba) {
    const auto lhs = normalize_integral(type, left);
    const auto rhs = normalize_integral(type, right);
    // Every branch below is arithmetically identical modulo 2^width; the
    // selector only changes which equivalent expression tree runs. A value
    // barrier on one sub-term stops the optimizer from folding the identity
    // back to the canonical operation.
    const auto barrier = [](const std::uint64_t value) {
        return handler_value_barrier(value);
    };
    switch (op) {
    case Op::add:
        switch (mba & 3) {
        case 1:
            return normalize_integral(
                type, lhs - barrier(std::uint64_t{0} - rhs));
        case 2:
            return normalize_integral(
                type, (lhs ^ rhs) + 2 * barrier(lhs & rhs));
        case 3:
            return normalize_integral(type, (lhs | rhs) + barrier(lhs & rhs));
        default:
            return normalize_integral(type, lhs + rhs);
        }
    case Op::subtract:
        switch (mba & 3) {
        case 1:
            return normalize_integral(
                type, lhs + barrier(std::uint64_t{0} - rhs));
        case 2:
            return normalize_integral(
                type, (lhs ^ rhs) - 2 * barrier((~lhs) & rhs));
        case 3:
            return normalize_integral(type, lhs + barrier(~rhs) + 1);
        default:
            return normalize_integral(type, lhs - rhs);
        }
    case Op::multiply:
        return normalize_integral(type, lhs * rhs);
    case Op::bit_and:
        switch (mba & 3) {
        case 1:
            return normalize_integral(type, (lhs | rhs) - barrier(lhs ^ rhs));
        case 2:
            return normalize_integral(type, lhs - barrier(lhs & ~rhs));
        default:
            return normalize_integral(type, lhs & rhs);
        }
    case Op::bit_or:
        switch (mba & 3) {
        case 1:
            return normalize_integral(type, barrier(lhs & rhs) + (lhs ^ rhs));
        case 2:
            return normalize_integral(type, lhs + barrier(rhs & ~lhs));
        default:
            return normalize_integral(type, lhs | rhs);
        }
    case Op::bit_xor:
        switch (mba & 3) {
        case 1:
            return normalize_integral(type, (lhs | rhs) - barrier(lhs & rhs));
        case 2:
            return normalize_integral(type, (lhs | rhs) & barrier(~(lhs & rhs)));
        default:
            return normalize_integral(type, lhs ^ rhs);
        }
    case Op::shift_left:
    case Op::shift_right: {
        const auto width = integral_width(type);
        if (rhs >= width) {
            throw std::runtime_error(
                xorstr_("Concept shift count is out of range"));
        }
        if (op == Op::shift_left) {
            return normalize_integral(type, lhs << rhs);
        }
        if (is_signed_integral(type)) {
            return normalize_integral(
                type, static_cast<std::uint64_t>(signed_value(type, lhs) >> rhs));
        }
        return normalize_integral(type, lhs >> rhs);
    }
    case Op::divide:
    case Op::modulo:
        if (rhs == 0) {
        throw std::runtime_error(
            op == Op::divide
                ? xorstr_("division by zero in Concept program")
                : xorstr_("modulo by zero in Concept program"));
        }
        if (!is_signed_integral(type)) {
            return normalize_integral(type, op == Op::divide ? lhs / rhs
                                                              : lhs % rhs);
        }
        {
            const auto signed_lhs = signed_value(type, lhs);
            const auto signed_rhs = signed_value(type, rhs);
            const auto width = integral_width(type);
            const auto minimum =
                width == 64
                    ? std::numeric_limits<std::int64_t>::min()
                    : -(std::int64_t{1} << (width - 1));
            if (signed_lhs == minimum && signed_rhs == -1) {
                return op == Op::divide ? normalize_integral(type, lhs) : 0;
            }
            const auto result = op == Op::divide ? signed_lhs / signed_rhs
                                                  : signed_lhs % signed_rhs;
            return normalize_integral(type,
                                      static_cast<std::uint64_t>(result));
        }
    default:
        throw std::runtime_error(
            xorstr_("invalid integral VM operation"));
    }
}

std::uint64_t arithmetic(const Op op, const ValueType type,
                         const std::uint64_t left,
                         const std::uint64_t right,
                         const std::uint8_t mba) {
    return is_floating(type) ? floating_arithmetic(op, type, left, right)
                             : integral_arithmetic(op, type, left, right, mba);
}

// ---------------------------------------------------------------------------
// Nested virtualization: selected integer operations are not computed directly
// but by interpreting a small inner "micro-program" on a second, self-contained
// interpreter with its own opcode set. The micro-program bytes are kept XOR
// masked and decoded one step at a time, so a dynamic observer sees an inner
// dispatch loop rather than a machine ADD. Results are identical to the direct
// handler; this only raises the cost of understanding the arithmetic.
// ---------------------------------------------------------------------------
enum class MicroOp : std::uint8_t {
    push_a,
    push_b,
    dup,
    add,
    sub,
    mul,
    bit_and,
    bit_or,
    bit_xor,
    negate,
    halt,
};

std::uint8_t micro_keystream(const std::uint64_t seed,
                             const std::size_t position) noexcept {
    return static_cast<std::uint8_t>(
        mix_handler_state(seed ^ (static_cast<std::uint64_t>(position) *
                                  0x9e3779b97f4a7c15ULL)) &
        0xff);
}

std::uint64_t run_micro_program(const std::uint8_t* encoded,
                                const std::size_t length,
                                const std::uint64_t a, const std::uint64_t b,
                                const std::uint64_t seed) {
    std::array<std::uint64_t, 16> stack{};
    std::size_t sp = 0;
    const auto push = [&](const std::uint64_t value) {
        if (sp >= stack.size()) {
            throw std::logic_error(xorstr_("nested VM stack overflow"));
        }
        stack[sp++] = value;
    };
    const auto pop = [&]() -> std::uint64_t {
        if (sp == 0) {
            throw std::logic_error(xorstr_("nested VM stack underflow"));
        }
        return stack[--sp];
    };
    for (std::size_t pc = 0; pc < length; ++pc) {
        switch (static_cast<MicroOp>(encoded[pc] ^ micro_keystream(seed, pc))) {
        case MicroOp::push_a:
            push(a);
            break;
        case MicroOp::push_b:
            push(b);
            break;
        case MicroOp::dup: {
            const auto top = pop();
            push(top);
            push(top);
            break;
        }
        case MicroOp::add: {
            const auto rhs = pop();
            push(pop() + rhs);
            break;
        }
        case MicroOp::sub: {
            const auto rhs = pop();
            push(pop() - rhs);
            break;
        }
        case MicroOp::mul: {
            const auto rhs = pop();
            push(pop() * rhs);
            break;
        }
        case MicroOp::bit_and: {
            const auto rhs = pop();
            push(pop() & rhs);
            break;
        }
        case MicroOp::bit_or: {
            const auto rhs = pop();
            push(pop() | rhs);
            break;
        }
        case MicroOp::bit_xor: {
            const auto rhs = pop();
            push(pop() ^ rhs);
            break;
        }
        case MicroOp::negate:
            push(std::uint64_t{0} - pop());
            break;
        case MicroOp::halt:
            return pop();
        }
    }
    return pop();
}

// Returns true and writes the result when `op` is virtualized through the
// inner interpreter for this instruction; false to fall back to the direct
// handler. Only integer add/sub/mul/and/or/xor participate.
bool nested_binary_op(const Op op, const ValueType type,
                      const std::uint64_t left, const std::uint64_t right,
                      const std::uint64_t seed, const std::uint8_t selector,
                      std::uint64_t& result) {
    const bool use_mba = (selector & 0x10) != 0;
    std::vector<MicroOp> program;
    switch (op) {
    case Op::add:
        program = use_mba ? std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::bit_xor, MicroOp::push_a,
                                                 MicroOp::push_b, MicroOp::bit_and,
                                                 MicroOp::dup, MicroOp::add,
                                                 MicroOp::add, MicroOp::halt}
                          : std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::add, MicroOp::halt};
        break;
    case Op::subtract:
        program = use_mba ? std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::negate, MicroOp::add,
                                                 MicroOp::halt}
                          : std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::sub, MicroOp::halt};
        break;
    case Op::multiply:
        program = {MicroOp::push_a, MicroOp::push_b, MicroOp::mul, MicroOp::halt};
        break;
    case Op::bit_and:
        program = use_mba ? std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::bit_or, MicroOp::push_a,
                                                 MicroOp::push_b, MicroOp::bit_xor,
                                                 MicroOp::sub, MicroOp::halt}
                          : std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::bit_and, MicroOp::halt};
        break;
    case Op::bit_or:
        program = use_mba ? std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::bit_and, MicroOp::push_a,
                                                 MicroOp::push_b, MicroOp::bit_xor,
                                                 MicroOp::add, MicroOp::halt}
                          : std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::bit_or, MicroOp::halt};
        break;
    case Op::bit_xor:
        program = use_mba ? std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::bit_or, MicroOp::push_a,
                                                 MicroOp::push_b, MicroOp::bit_and,
                                                 MicroOp::sub, MicroOp::halt}
                          : std::vector<MicroOp>{MicroOp::push_a, MicroOp::push_b,
                                                 MicroOp::bit_xor, MicroOp::halt};
        break;
    default:
        return false;
    }

    std::vector<std::uint8_t> encoded(program.size());
    for (std::size_t index = 0; index < program.size(); ++index) {
        encoded[index] = static_cast<std::uint8_t>(program[index]) ^
                         micro_keystream(seed, index);
    }
    result = normalize_integral(
        type, run_micro_program(encoded.data(), encoded.size(),
                                normalize_integral(type, left),
                                normalize_integral(type, right), seed));
    return true;
}

bool compare_values_canonical(const Op op, const ValueType type,
                              const std::uint64_t left,
                              const std::uint64_t right) {
    if (type == ValueType::boolean) {
        const bool lhs = left != 0;
        const bool rhs = right != 0;
        return op == Op::equal ? lhs == rhs : lhs != rhs;
    }
    if (type == ValueType::f32) {
        const auto lhs = f32_value(left);
        const auto rhs = f32_value(right);
        switch (op) {
        case Op::equal:
            return lhs == rhs;
        case Op::not_equal:
            return lhs != rhs;
        case Op::less:
            return lhs < rhs;
        case Op::less_equal:
            return lhs <= rhs;
        case Op::greater:
            return lhs > rhs;
        case Op::greater_equal:
            return lhs >= rhs;
        default:
            break;
        }
    } else if (type == ValueType::f64) {
        const auto lhs = f64_value(left);
        const auto rhs = f64_value(right);
        switch (op) {
        case Op::equal:
            return lhs == rhs;
        case Op::not_equal:
            return lhs != rhs;
        case Op::less:
            return lhs < rhs;
        case Op::less_equal:
            return lhs <= rhs;
        case Op::greater:
            return lhs > rhs;
        case Op::greater_equal:
            return lhs >= rhs;
        default:
            break;
        }
    } else if (is_signed_integral(type)) {
        const auto lhs = signed_value(type, left);
        const auto rhs = signed_value(type, right);
        switch (op) {
        case Op::equal:
            return lhs == rhs;
        case Op::not_equal:
            return lhs != rhs;
        case Op::less:
            return lhs < rhs;
        case Op::less_equal:
            return lhs <= rhs;
        case Op::greater:
            return lhs > rhs;
        case Op::greater_equal:
            return lhs >= rhs;
        default:
            break;
        }
    } else {
        const auto lhs = normalize_integral(type, left);
        const auto rhs = normalize_integral(type, right);
        switch (op) {
        case Op::equal:
            return lhs == rhs;
        case Op::not_equal:
            return lhs != rhs;
        case Op::less:
            return lhs < rhs;
        case Op::less_equal:
            return lhs <= rhs;
        case Op::greater:
            return lhs > rhs;
        case Op::greater_equal:
            return lhs >= rhs;
        default:
            break;
        }
    }
    throw std::runtime_error(
        xorstr_("invalid comparison VM operation"));
}

Op mirrored_comparison(const Op op) {
    switch (op) {
    case Op::equal:
    case Op::not_equal:
        return op;
    case Op::less:
        return Op::greater;
    case Op::less_equal:
        return Op::greater_equal;
    case Op::greater:
        return Op::less;
    case Op::greater_equal:
        return Op::less_equal;
    default:
        throw std::logic_error(xorstr_(
            "invalid comparison handler substitution"));
    }
}

bool compare_values(const Op op, const ValueType type,
                    const std::uint64_t left, const std::uint64_t right,
                    const bool substituted) {
    return substituted
               ? compare_values_canonical(mirrored_comparison(op), type,
                                          handler_value_barrier(right),
                                          handler_value_barrier(left))
               : compare_values_canonical(op, type, left, right);
}

std::uint64_t negate_value(const ValueType type, const std::uint64_t bits) {
    if (type == ValueType::f32) {
        return f32_bits(-f32_value(bits));
    }
    if (type == ValueType::f64) {
        return f64_bits(-f64_value(bits));
    }
    return normalize_integral(type, std::uint64_t{0} - bits);
}

// String handles are opaque 64-bit values on the operand stack. The top bit
// tags a handle as an immutable string constant whose remaining bits index the
// bytecode's constant table; without the bit the handle indexes the dynamic
// text heap. Tagging constants keeps them zero-copy so repeatedly pushing the
// same literal (for example inside a loop) neither copies nor grows the heap.
constexpr std::uint64_t text_constant_flag = std::uint64_t{1} << 63;

const std::string& text_value(const std::vector<std::string>& heap,
                              const std::vector<std::string>& constants,
                              const std::uint64_t handle) {
    if ((handle & text_constant_flag) != 0) {
        const auto index = handle & ~text_constant_flag;
        if (index >= constants.size()) {
            throw std::runtime_error(
                xorstr_("Concept VM string handle is invalid"));
        }
        return constants[static_cast<std::size_t>(index)];
    }
    if (handle >= heap.size()) {
        throw std::runtime_error(
            xorstr_("Concept VM string handle is invalid"));
    }
    return heap[static_cast<std::size_t>(handle)];
}

std::string read_input_line() {
    std::string line;
    if (!std::getline(std::cin, line)) {
        throw std::runtime_error(
            xorstr_("failed to read a line from standard input"));
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

std::string_view trim(const std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::int64_t read_input_i64() {
    const auto line = read_input_line();
    const auto value_text = trim(line);
    std::int64_t value = 0;
    const auto result = std::from_chars(value_text.data(),
                                        value_text.data() + value_text.size(),
                                        value);
    if (value_text.empty() || result.ec != std::errc{} ||
        result.ptr != value_text.data() + value_text.size()) {
        throw std::runtime_error(
            xorstr_("standard input is not a valid i64 value"));
    }
    return value;
}

double read_input_f64() {
    const auto line = read_input_line();
    const auto value_text = trim(line);
    double value = 0.0;
    const auto result = std::from_chars(value_text.data(),
                                        value_text.data() + value_text.size(),
                                        value);
    if (value_text.empty() || result.ec != std::errc{} ||
        result.ptr != value_text.data() + value_text.size()) {
        throw std::runtime_error(
            xorstr_("standard input is not a valid f64 value"));
    }
    return value;
}

void print_value(const ValueType type, const std::uint64_t bits,
                 const std::vector<std::string>& heap,
                 const std::vector<std::string>& constants,
                 const bool newline) {
    switch (type) {
    case ValueType::boolean:
        std::cout << (bits == 0 ? xorstr_("false") : xorstr_("true"));
        break;
    case ValueType::i8:
    case ValueType::i16:
    case ValueType::i32:
    case ValueType::i64:
        std::cout << signed_value(type, bits);
        break;
    case ValueType::u8:
    case ValueType::u16:
    case ValueType::u32:
    case ValueType::u64:
        std::cout << normalize_integral(type, bits);
        break;
    case ValueType::f32:
        std::cout << f32_value(bits);
        break;
    case ValueType::f64:
        std::cout << f64_value(bits);
        break;
    case ValueType::text:
        std::cout << text_value(heap, constants, bits);
        break;
    case ValueType::void_type:
        throw std::runtime_error(
            xorstr_("Concept VM cannot print a void value"));
    }
    if (newline) {
        std::cout << '\n';
    }
    std::cout.flush();
}

std::int64_t exit_value(const ValueType type, const std::uint64_t bits) {
    if (type == ValueType::boolean) {
        return bits == 0 ? 0 : 1;
    }
    if (is_signed_integral(type)) {
        return signed_value(type, bits);
    }
    if (is_integral(type)) {
        const auto value = normalize_integral(type, bits);
        return type == ValueType::u64 ? std::bit_cast<std::int64_t>(value)
                                      : static_cast<std::int64_t>(value);
    }
    throw std::runtime_error(xorstr_(
        "Concept entry point returned a non-integral value"));
}

} // namespace

std::int64_t execute(const Bytecode& bytecode) {
    validate(bytecode);

    struct VmContext {
        std::uint32_t begin{};
        std::uint32_t end{};
        std::uint64_t opcode_seed{};
        bool reversed{};
        std::span<const std::uint8_t> owned_code;
    };

    constexpr std::size_t maximum_call_depth = 4096;
    std::vector<std::uint64_t> stack;
    std::vector<Frame> frames;
    std::vector<std::string> text_heap;
    std::vector<std::vector<std::uint64_t>> object_heap;
    std::vector<PointerTarget> pointer_heap;
    std::vector<HeapBlock> heap_blocks;
    std::unordered_map<HeapPointerKey, std::uint64_t, HeapPointerKeyHash>
        heap_pointer_handles;
    std::uint64_t next_frame_id = 1;
    text_heap.emplace_back();
    frames.push_back(
        {next_frame_id++, bytecode.code.size(), 0,
         std::vector<std::uint64_t>(bytecode.entry_locals, 0)});
    std::size_t ip = bytecode.entry;
    std::vector<VmContext> vm_contexts;
    if (bytecode.vm_regions.empty()) {
        const auto seed = bytecode.vm_seeds.empty()
                              ? std::uint64_t{0}
                              : bytecode.vm_seeds.front();
        vm_contexts.push_back(
            {0, static_cast<std::uint32_t>(bytecode.code.size()), seed, false,
             bytecode.code});
    } else {
        vm_contexts.reserve(bytecode.vm_regions.size());
        for (const auto& region : bytecode.vm_regions) {
            vm_contexts.push_back(
                {region.begin, region.end, region.opcode_seed,
                 (region.opcode_seed & (std::uint64_t{1} << 63)) != 0,
                 std::span<const std::uint8_t>(bytecode.code)
                     .subspan(region.begin, region.end - region.begin)});
        }
    }
    std::size_t active_vm = 0;

    const auto physical_address = [&](const std::size_t logical_offset) {
        if (logical_offset == bytecode.code.size()) {
            return logical_offset;
        }
        const auto context = std::find_if(
            vm_contexts.begin(), vm_contexts.end(),
            [&](const VmContext& candidate) {
                return logical_offset >= candidate.begin &&
                       logical_offset < candidate.end;
            });
        if (context == vm_contexts.end()) {
            throw std::runtime_error(xorstr_(
                "Concept logical instruction belongs to no VM context"));
        }
        return context->reversed
                   ? static_cast<std::size_t>(context->begin) + context->end -
                         1 - logical_offset
                   : logical_offset;
    };
    ip = physical_address(bytecode.entry);

    const auto select_vm = [&]() -> const VmContext& {
        const auto contains_ip = [&](const VmContext& context) {
            return ip >= context.begin && ip < context.end;
        };
        if (contains_ip(vm_contexts[active_vm])) {
            return vm_contexts[active_vm];
        }
        for (std::size_t index = 0; index < vm_contexts.size(); ++index) {
            if (contains_ip(vm_contexts[index])) {
                active_vm = index;
                return vm_contexts[index];
            }
        }
        throw std::runtime_error(xorstr_(
            "Concept instruction pointer belongs to no VM context"));
    };

    // Operand-stack values are kept XOR-masked in memory with a per-run key so
    // a raw stack dump does not reveal plaintext intermediates. The mask is a
    // function of the absolute slot index, so it survives truncation on return
    // and is applied/removed only transiently as values are pushed and popped.
    const std::uint64_t stack_mask_key = [&] {
        std::random_device device;
        auto key = (static_cast<std::uint64_t>(device()) << 32) ^
                   static_cast<std::uint64_t>(device());
        key ^= static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(&stack));
        return mix_handler_state(key ^ 0x243f6a8885a308d3ULL);
    }();
    const auto stack_mask = [&](const std::size_t index) {
        return mix_handler_state(stack_mask_key ^
                                 (static_cast<std::uint64_t>(index) *
                                  0x9e3779b97f4a7c15ULL));
    };
    const auto push_value = [&](const std::uint64_t value) {
        stack.push_back(value ^ stack_mask(stack.size()));
    };

    const auto pop = [&]() {
        if (stack.empty() || stack.size() <= frames.back().stack_base) {
            throw std::runtime_error(
                xorstr_("Concept VM operand stack underflow"));
        }
        const auto value = stack.back() ^ stack_mask(stack.size() - 1);
        stack.pop_back();
        return value;
    };

    // Per-run key and derivations for masking frame locals in memory. The raw
    // mask is keyed by the frame id and slot; the effective mask is zero for an
    // unobfuscated frame or a pinned (address-taken) slot, which are stored in
    // the clear.
    const std::uint64_t session_local_key = [&] {
        std::random_device device;
        auto key = (static_cast<std::uint64_t>(device()) << 32) ^
                   static_cast<std::uint64_t>(device());
        key ^= static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(&frames));
        return mix_handler_state(key ^ 0xa5a5a5a5c3c3c3c3ULL);
    }();
    const auto local_raw_mask = [&](const std::uint64_t frame_id,
                                    const std::uint64_t index) {
        return mix_handler_state(session_local_key ^ frame_id ^
                                 (index * 0x9e3779b97f4a7c15ULL));
    };
    const auto local_effective_mask =
        [&](const Frame& frame, const std::uint64_t index) -> std::uint64_t {
        if (frame.complexity == 0) {
            return 0;
        }
        if (index < frame.local_pinned.size() &&
            frame.local_pinned[static_cast<std::size_t>(index)]) {
            return 0;
        }
        return local_raw_mask(frame.id, index);
    };

    const auto pop_binary = [&](const bool reordered) {
        if (!reordered) {
            const auto right = pop();
            const auto left = pop();
            return std::pair{left, right};
        }
        if (stack.size() < frames.back().stack_base ||
            stack.size() - frames.back().stack_base < 2) {
            throw std::runtime_error(
                xorstr_("Concept VM operand stack underflow"));
        }
        const auto left =
            stack[stack.size() - 2] ^ stack_mask(stack.size() - 2);
        const auto right = stack.back() ^ stack_mask(stack.size() - 1);
        stack.resize(stack.size() - 2);
        return std::pair{left, right};
    };

    const auto transfer_arguments = [&](std::vector<std::uint64_t>& locals,
                                        const std::size_t local_offset,
                                        const std::size_t argument_count,
                                        const bool reordered) {
        if (!reordered) {
            for (std::size_t index = argument_count; index != 0; --index) {
                locals[local_offset + index - 1] = pop();
            }
            return;
        }
        if (stack.size() < frames.back().stack_base ||
            argument_count > stack.size() - frames.back().stack_base) {
            throw std::runtime_error(
                xorstr_("Concept VM operand stack underflow"));
        }
        const auto base = stack.size() - argument_count;
        for (std::size_t index = 0; index < argument_count; ++index) {
            locals[local_offset + index] =
                stack[base + index] ^ stack_mask(base + index);
        }
        stack.resize(base);
    };

    const auto object_fields = [&](const std::uint64_t handle)
        -> std::vector<std::uint64_t>& {
        if (handle == 0 || handle > object_heap.size()) {
            throw std::runtime_error(xorstr_(
                "Concept VM class object handle is invalid"));
        }
        return object_heap[static_cast<std::size_t>(handle - 1)];
    };

    const auto pointer_target = [&](const std::uint64_t handle)
        -> const PointerTarget& {
        if (handle == 0 || handle > pointer_heap.size()) {
            throw std::runtime_error(
                xorstr_("Concept VM pointer is null or invalid"));
        }
        return pointer_heap[static_cast<std::size_t>(handle - 1)];
    };

    const auto heap_block = [&](const std::uint64_t handle) -> HeapBlock& {
        if (handle == 0 || handle > heap_blocks.size()) {
            throw std::runtime_error(
                xorstr_("Concept VM heap block handle is invalid"));
        }
        auto& block = heap_blocks[static_cast<std::size_t>(handle - 1)];
        if (block.freed) {
            throw std::runtime_error(
                xorstr_("Concept VM heap pointer refers to freed memory"));
        }
        if (!block.dynamic && block.frame_id != 0 &&
            std::none_of(frames.begin(), frames.end(),
                         [&](const Frame& frame) {
                             return frame.id == block.frame_id;
                         })) {
            throw std::runtime_error(xorstr_(
                "Concept VM pointer refers to an expired local array"));
        }
        return block;
    };

    const auto local_frame = [&](const std::uint64_t id) -> Frame& {
        const auto frame = std::find_if(
            frames.rbegin(), frames.rend(),
            [id](const Frame& candidate) { return candidate.id == id; });
        if (frame == frames.rend()) {
            throw std::runtime_error(xorstr_(
                "Concept VM pointer refers to an expired local variable"));
        }
        return *frame;
    };

    const auto native_pointer_address = [&](const std::uint64_t handle) {
        if (handle == 0) {
            return std::uintptr_t{0};
        }
        const auto& target = pointer_target(handle);
        if (target.kind == PointerTarget::Kind::local) {
            auto& frame = local_frame(target.owner);
            if (target.index >= frame.locals.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept native-call local pointer is out of range"));
            }
            return reinterpret_cast<std::uintptr_t>(
                &frame.locals[static_cast<std::size_t>(target.index)]);
        }
        if (target.kind == PointerTarget::Kind::field) {
            auto& fields = object_fields(target.owner);
            if (target.index >= fields.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept native-call field pointer is out of range"));
            }
            return reinterpret_cast<std::uintptr_t>(
                &fields[static_cast<std::size_t>(target.index)]);
        }
        if (target.kind == PointerTarget::Kind::heap_block) {
            auto& block = heap_block(target.owner);
            if (target.index > block.bytes.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept native-call heap pointer is out of range"));
            }
            return reinterpret_cast<std::uintptr_t>(
                block.bytes.data() + target.index);
        }
        return static_cast<std::uintptr_t>(target.owner);
    };

    const auto offset_pointer_target = [&](const std::uint64_t handle,
                                           const std::uint64_t offset_bits,
                                           const ValueType type) {
        const auto offset = std::bit_cast<std::int64_t>(offset_bits);
        auto adjusted = pointer_target(handle);
        if (adjusted.kind == PointerTarget::Kind::local ||
            adjusted.kind == PointerTarget::Kind::field) {
            if (offset != 0) {
                throw std::runtime_error(xorstr_(
                    "Concept VM cannot offset a local or field pointer"));
            }
            return adjusted;
        }
        if (adjusted.native_type != type) {
            throw std::runtime_error(xorstr_(
                "Concept VM pointer arithmetic type mismatch"));
        }
        const auto element_size = heap_value_size(type);
        const auto magnitude =
            offset < 0 ? std::uint64_t{0} -
                             static_cast<std::uint64_t>(offset)
                       : static_cast<std::uint64_t>(offset);
        if (magnitude > std::numeric_limits<std::uint64_t>::max() /
                            element_size) {
            throw std::runtime_error(
                xorstr_("Concept VM pointer offset overflow"));
        }
        const auto byte_delta = magnitude * element_size;
        auto& position = adjusted.kind == PointerTarget::Kind::heap_block
                             ? adjusted.index
                             : adjusted.owner;
        if (offset < 0) {
            if (byte_delta > position) {
                throw std::runtime_error(xorstr_(
                    "Concept VM pointer offset is out of bounds"));
            }
            position -= byte_delta;
        } else {
            if (byte_delta >
                std::numeric_limits<std::uint64_t>::max() - position) {
                throw std::runtime_error(
                    xorstr_("Concept VM pointer offset overflow"));
            }
            position += byte_delta;
        }
        if (adjusted.kind == PointerTarget::Kind::heap_block) {
            auto& block = heap_block(adjusted.owner);
            if (adjusted.index > block.bytes.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept VM pointer offset is out of bounds"));
            }
        }
        return adjusted;
    };

    const auto load_pointer_target = [&](const PointerTarget& target) {
        if (target.kind == PointerTarget::Kind::local) {
            auto& frame = local_frame(target.owner);
            if (target.index >= frame.locals.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept VM pointer local index is out of range"));
            }
            return frame.locals[target.index];
        }
        if (target.kind == PointerTarget::Kind::field) {
            auto& fields = object_fields(target.owner);
            if (target.index >= fields.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept VM pointer field index is out of range"));
            }
            return fields[target.index];
        }
        if (target.kind == PointerTarget::Kind::heap_block) {
            auto& block = heap_block(target.owner);
            const auto size = heap_value_size(target.native_type);
            if (target.index > block.bytes.size() ||
                size > block.bytes.size() -
                           static_cast<std::size_t>(target.index)) {
                throw std::runtime_error(xorstr_(
                    "Concept VM heap pointer is out of bounds"));
            }
            std::uint64_t value = 0;
            std::memcpy(&value, block.bytes.data() + target.index, size);
            return target.native_type == ValueType::boolean
                       ? (value == 0 ? 0ULL : 1ULL)
                       : value;
        }
        std::uint64_t value = 0;
        if (!read_native_value(target.owner, target.native_type, value)) {
            throw std::runtime_error(xorstr_(
                "Concept VM could not read the native pointer address"));
        }
        return target.native_type == ValueType::boolean
                   ? (value == 0 ? 0ULL : 1ULL)
                   : value;
    };

    const auto load_pointer = [&](const std::uint64_t handle) {
        return load_pointer_target(pointer_target(handle));
    };

    const auto store_pointer_target = [&](const PointerTarget& target,
                                          const std::uint64_t value) {
        if (target.kind == PointerTarget::Kind::local) {
            auto& frame = local_frame(target.owner);
            if (target.index >= frame.locals.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept VM pointer local index is out of range"));
            }
            frame.locals[target.index] = value;
            return;
        }
        if (target.kind == PointerTarget::Kind::field) {
            auto& fields = object_fields(target.owner);
            if (target.index >= fields.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept VM pointer field index is out of range"));
            }
            fields[target.index] = value;
            return;
        }
        if (target.kind == PointerTarget::Kind::heap_block) {
            auto& block = heap_block(target.owner);
            const auto size = heap_value_size(target.native_type);
            if (target.index > block.bytes.size() ||
                size > block.bytes.size() -
                           static_cast<std::size_t>(target.index)) {
                throw std::runtime_error(xorstr_(
                    "Concept VM heap pointer is out of bounds"));
            }
            const auto stored = target.native_type == ValueType::boolean
                                    ? (value == 0 ? 0ULL : 1ULL)
                                    : value;
            std::memcpy(block.bytes.data() + target.index, &stored, size);
            return;
        }
        const auto stored = target.native_type == ValueType::boolean
                                ? (value == 0 ? 0ULL : 1ULL)
                                : value;
        if (!write_native_value(target.owner, target.native_type, stored)) {
            throw std::runtime_error(xorstr_(
                "Concept VM could not write the native pointer address"));
        }
    };

    const auto store_pointer = [&](const std::uint64_t handle,
                                   const std::uint64_t value) {
        store_pointer_target(pointer_target(handle), value);
    };

    const auto byte_span = [&](const std::uint64_t handle,
                               const std::uint64_t count) {
        if (count == 0) {
            return std::span<std::uint8_t>{};
        }
        if (count > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error(
                xorstr_("Concept binary buffer length is too large"));
        }
        const auto& target = pointer_target(handle);
        if (target.kind != PointerTarget::Kind::heap_block ||
            target.native_type != ValueType::u8) {
            throw std::runtime_error(
                xorstr_("Concept binary buffer requires VM u8 storage"));
        }
        auto& block = heap_block(target.owner);
        const auto size = static_cast<std::size_t>(count);
        if (target.index > block.bytes.size() ||
            size > block.bytes.size() -
                       static_cast<std::size_t>(target.index)) {
            throw std::runtime_error(
                xorstr_("Concept binary buffer is out of bounds"));
        }
        return std::span<std::uint8_t>(
            block.bytes.data() + static_cast<std::size_t>(target.index), size);
    };

    for (;;) {
        const auto& vm = select_vm();
        const auto instruction_offset = ip;
        const auto local_ip = ip - vm.begin;
        const auto op = static_cast<Op>(vm.owned_code[local_ip]);
        const auto logical_instruction =
            vm.reversed
                ? static_cast<std::size_t>(vm.begin) + vm.end - 1 - ip
                : ip;
        const auto instruction_size = 1 + operand_size(op);
        const auto next_logical_instruction =
            logical_instruction + instruction_size;
        if (next_logical_instruction > vm.end) {
            throw std::runtime_error(
                xorstr_("VM instruction crosses its bytecode region"));
        }
        OperandReader operands(bytecode.code, instruction_offset, vm.begin,
                               vm.end, vm.reversed);
        if (next_logical_instruction < vm.end) {
            ip = vm.reversed ? instruction_offset - instruction_size
                             : instruction_offset + instruction_size;
        } else {
            ip = physical_address(next_logical_instruction);
        }
        std::uint8_t variant = 0;
        std::uint8_t mba = 0;
        if (frames.back().complexity != 0) {
            variant = handler_variant(vm.opcode_seed, op);
            mba = handler_mba(vm.opcode_seed, op, instruction_offset);
            execute_handler_junk(
                vm.opcode_seed, op, instruction_offset, variant);
        }
        const bool reordered = (variant & 1) != 0;
        const bool substituted = (variant & 2) != 0;
        // Route selected integer ops through the inner interpreter (only under
        // obfuscation; mba is 0 for an unobfuscated frame).
        const bool nested = (mba & 0x20) != 0;

        switch (op) {
        case Op::push_bits: {
            auto value = operands.read_u64();
            if (bytecode.immediates_masked) {
                value ^= immediate_mask(vm.opcode_seed, logical_instruction);
            }
            push_value(value);
            break;
        }
        case Op::push_text: {
            const auto index = operands.read_u32();
            if (index >= bytecode.strings.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept VM string constant is out of range"));
            }
            push_value(text_constant_flag | index);
            break;
        }
        case Op::load: {
            const auto index = operands.read_u16();
            if (index >= frames.back().locals.size()) {
                throw std::runtime_error(
                    xorstr_("Concept VM local load is out of range"));
            }
            push_value(frames.back().locals[index] ^
                       local_effective_mask(frames.back(), index));
            break;
        }
        case Op::store: {
            const auto index = operands.read_u16();
            if (reordered) {
                if (index >= frames.back().locals.size()) {
                    throw std::runtime_error(
                        xorstr_("Concept VM local store is out of range"));
                }
                frames.back().locals[index] =
                    pop() ^ local_effective_mask(frames.back(), index);
            } else {
                const auto value = pop();
                if (index >= frames.back().locals.size()) {
                    throw std::runtime_error(
                        xorstr_("Concept VM local store is out of range"));
                }
                frames.back().locals[index] =
                    value ^ local_effective_mask(frames.back(), index);
            }
            break;
        }
        case Op::new_object: {
            const auto field_count = operands.read_u16();
            object_heap.emplace_back(field_count, 0);
            push_value(object_heap.size());
            break;
        }
        case Op::load_field: {
            const auto index = operands.read_u16();
            const auto& fields = object_fields(pop());
            if (index >= fields.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept VM class field load is out of range"));
            }
            push_value(fields[index]);
            break;
        }
        case Op::store_field: {
            const auto index = operands.read_u16();
            const auto [object, value] = pop_binary(reordered);
            auto& fields = object_fields(object);
            if (index >= fields.size()) {
                throw std::runtime_error(xorstr_(
                    "Concept VM class field store is out of range"));
            }
            fields[index] = value;
            break;
        }
        case Op::address_local: {
            const auto index = operands.read_u16();
            if (index >= frames.back().locals.size()) {
                throw std::runtime_error(
                    xorstr_("Concept VM local address is out of range"));
            }
            // Pin the slot: unmask it in place so pointer and native access
            // see plaintext storage for the life of the frame.
            auto& frame = frames.back();
            if (frame.complexity != 0) {
                if (frame.local_pinned.size() < frame.locals.size()) {
                    frame.local_pinned.resize(frame.locals.size(), false);
                }
                if (!frame.local_pinned[index]) {
                    frame.locals[index] ^= local_raw_mask(frame.id, index);
                    frame.local_pinned[index] = true;
                }
            }
            pointer_heap.push_back(
                {PointerTarget::Kind::local, frame.id, index});
            push_value(pointer_heap.size());
            break;
        }
        case Op::address_field: {
            const auto index = operands.read_u16();
            const auto object = pop();
            auto& fields = object_fields(object);
            if (index >= fields.size()) {
                throw std::runtime_error(
                    xorstr_("Concept VM field address is out of range"));
            }
            pointer_heap.push_back(
                {PointerTarget::Kind::field, object, index});
            push_value(pointer_heap.size());
            break;
        }
        case Op::native_pointer: {
            const auto type = operands.read_type();
            const auto address = pop();
            if (address == 0) {
                push_value(0);
                break;
            }
            pointer_heap.push_back(
                {PointerTarget::Kind::native_address, address, 0, type});
            push_value(pointer_heap.size());
            break;
        }
        case Op::array_alloc: {
            const auto type = operands.read_type();
            const auto count = operands.read_u32();
            const auto element_size = heap_value_size(type);
            if (count >
                std::numeric_limits<std::size_t>::max() / element_size) {
                throw std::runtime_error(
                    xorstr_("Concept VM array allocation is too large"));
            }
            heap_blocks.push_back(
                {std::vector<std::uint8_t>(
                     static_cast<std::size_t>(count) * element_size, 0),
                 false, false, frames.back().id});
            pointer_heap.push_back({PointerTarget::Kind::heap_block,
                                    heap_blocks.size(), 0, type});
            const auto handle = pointer_heap.size();
            heap_pointer_handles.emplace(
                HeapPointerKey{heap_blocks.size(), 0, type}, handle);
            push_value(handle);
            break;
        }
        case Op::heap_alloc: {
            const auto type = operands.read_type();
            const auto byte_count = pop();
            if (byte_count > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error(
                    xorstr_("Concept VM heap allocation is too large"));
            }
            heap_blocks.push_back(
                {std::vector<std::uint8_t>(
                     static_cast<std::size_t>(byte_count), 0),
                 true, false, 0});
            pointer_heap.push_back({PointerTarget::Kind::heap_block,
                                    heap_blocks.size(), 0, type});
            const auto handle = pointer_heap.size();
            heap_pointer_handles.emplace(
                HeapPointerKey{heap_blocks.size(), 0, type}, handle);
            push_value(handle);
            break;
        }
        case Op::heap_free: {
            const auto pointer = pop();
            if (pointer == 0) {
                push_value(0);
                break;
            }
            const auto target = pointer_target(pointer);
            if (target.kind != PointerTarget::Kind::heap_block ||
                target.index != 0) {
                throw std::runtime_error(
                    xorstr_("Concept VM free requires a malloc base pointer"));
            }
            if (target.owner == 0 || target.owner > heap_blocks.size()) {
                throw std::runtime_error(
                    xorstr_("Concept VM heap block handle is invalid"));
            }
            auto& block =
                heap_blocks[static_cast<std::size_t>(target.owner - 1)];
            if (!block.dynamic) {
                throw std::runtime_error(
                    xorstr_("Concept VM cannot free fixed array storage"));
            }
            if (block.freed) {
                throw std::runtime_error(
                    xorstr_("Concept VM detected a double free"));
            }
            block.bytes.clear();
            block.freed = true;
            push_value(0);
            break;
        }
        case Op::pointer_offset: {
            const auto type = operands.read_type();
            const auto [handle, offset_bits] = pop_binary(reordered);
            const auto adjusted =
                offset_pointer_target(handle, offset_bits, type);
            if (adjusted.kind == PointerTarget::Kind::local ||
                adjusted.kind == PointerTarget::Kind::field) {
                push_value(handle);
                break;
            }
            if (adjusted.kind == PointerTarget::Kind::heap_block) {
                const HeapPointerKey key{
                    adjusted.owner, adjusted.index, adjusted.native_type};
                const auto existing = heap_pointer_handles.find(key);
                if (existing != heap_pointer_handles.end()) {
                    push_value(existing->second);
                    break;
                }
                pointer_heap.push_back(adjusted);
                const auto cached_handle = pointer_heap.size();
                heap_pointer_handles.emplace(key, cached_handle);
                push_value(cached_handle);
                break;
            }
            pointer_heap.push_back(adjusted);
            push_value(pointer_heap.size());
            break;
        }
        case Op::load_indirect:
            push_value(load_pointer(pop()));
            break;
        case Op::store_indirect: {
            const auto [pointer, value] = pop_binary(reordered);
            store_pointer(pointer, value);
            break;
        }
        case Op::load_indexed: {
            const auto type = operands.read_type();
            const auto offset = pop();
            const auto pointer = pop();
            const auto target =
                offset_pointer_target(pointer, offset, type);
            push_value(load_pointer_target(target));
            break;
        }
        case Op::store_indexed: {
            const auto type = operands.read_type();
            const auto value = pop();
            const auto offset = pop();
            const auto pointer = pop();
            const auto target =
                offset_pointer_target(pointer, offset, type);
            store_pointer_target(target, value);
            break;
        }
        case Op::pop:
            static_cast<void>(pop());
            break;
        case Op::convert: {
            const auto source = operands.read_type();
            const auto target = operands.read_type();
            push_value(convert_value(pop(), source, target));
            break;
        }
        case Op::add:
        case Op::subtract:
        case Op::multiply:
        case Op::divide:
        case Op::modulo: {
            const auto type = operands.read_type();
            const auto [left, right] = pop_binary(reordered);
            std::uint64_t nested_result = 0;
            if (nested && !is_floating(type) &&
                nested_binary_op(op, type, left, right, vm.opcode_seed, mba,
                                 nested_result)) {
                push_value(nested_result);
            } else {
                push_value(arithmetic(op, type, left, right, mba));
            }
            break;
        }
        case Op::bit_and:
        case Op::bit_or:
        case Op::bit_xor:
        case Op::shift_left:
        case Op::shift_right: {
            const auto type = operands.read_type();
            const auto [left, right] = pop_binary(reordered);
            std::uint64_t nested_result = 0;
            if (nested && nested_binary_op(op, type, left, right,
                                           vm.opcode_seed, mba,
                                           nested_result)) {
                push_value(nested_result);
            } else {
                push_value(integral_arithmetic(op, type, left, right, mba));
            }
            break;
        }
        case Op::bit_not: {
            const auto type = operands.read_type();
            push_value(normalize_integral(type, ~pop()));
            break;
        }
        case Op::negate: {
            const auto type = operands.read_type();
            const auto value = pop();
            push_value(substituted && !is_floating(type)
                                ? normalize_integral(
                                      type,
                                      handler_value_barrier(~value) + 1)
                                : negate_value(type, value));
            break;
        }
        case Op::logical_not: {
            const auto type = operands.read_type();
            const auto value = truthy(type, pop());
            push_value(substituted
                                ? static_cast<std::uint64_t>(!value)
                                : (value ? 0 : 1));
            break;
        }
        case Op::equal:
        case Op::not_equal:
        case Op::less:
        case Op::less_equal:
        case Op::greater:
        case Op::greater_equal: {
            const auto type = operands.read_type();
            const auto [left, right] = pop_binary(reordered);
            if (type == ValueType::text) {
                if (op != Op::equal && op != Op::not_equal) {
                    throw std::runtime_error(xorstr_(
                        "Concept VM string ordering is not supported"));
                }
                const bool equal = substituted
                                       ? text_value(text_heap, bytecode.strings, right) ==
                                             text_value(text_heap, bytecode.strings, left)
                                       : text_value(text_heap, bytecode.strings, left) ==
                                             text_value(text_heap, bytecode.strings, right);
                push_value((op == Op::equal ? equal : !equal) ? 1 : 0);
            } else {
                push_value(compare_values(op, type, left, right,
                                               substituted)
                                    ? 1
                                    : 0);
            }
            break;
        }
        case Op::jump:
            ip = physical_address(operands.read_u32());
            break;
        case Op::jump_if_false: {
            std::uint32_t target{};
            std::uint64_t condition{};
            if (reordered) {
                condition = pop();
                target = operands.read_u32();
            } else {
                target = operands.read_u32();
                condition = pop();
            }
            const bool should_jump = substituted ? !condition : condition == 0;
            if (should_jump) {
                ip = physical_address(target);
            }
            break;
        }
        case Op::call: {
            const auto target = operands.read_u32();
            const auto local_count = operands.read_u32();
            const auto argument_count = operands.read_u16();
            if (frames.size() >= maximum_call_depth) {
                throw std::runtime_error(
                    xorstr_("Concept VM call stack overflow"));
            }
            if (argument_count > local_count) {
                throw std::runtime_error(xorstr_(
                    "Concept VM call has more arguments than locals"));
            }
            std::vector<std::uint64_t> function_locals(local_count, 0);
            transfer_arguments(function_locals, 0, argument_count,
                               reordered);
            frames.push_back({next_frame_id++, ip, stack.size(),
                              std::move(function_locals)});
            ip = physical_address(target);
            break;
        }
        case Op::call_method:
        case Op::call_constructor: {
            const auto target = operands.read_u32();
            const auto local_count = operands.read_u32();
            const auto argument_count = operands.read_u16();
            if (frames.size() >= maximum_call_depth) {
                throw std::runtime_error(
                    xorstr_("Concept VM call stack overflow"));
            }
            if (local_count == 0 || argument_count >= local_count) {
                throw std::runtime_error(
                    xorstr_("Concept VM method call local layout is invalid"));
            }
            std::vector<std::uint64_t> method_locals(local_count, 0);
            transfer_arguments(method_locals, 1, argument_count,
                               reordered);
            const auto receiver = pop();
            method_locals[0] = receiver;
            frames.push_back({next_frame_id++, ip, stack.size(),
                              std::move(method_locals),
                              op == Op::call_constructor ? receiver : 0});
            ip = physical_address(target);
            break;
        }
        case Op::input_text:
            text_heap.push_back(read_input_line());
            push_value(text_heap.size() - 1);
            break;
        case Op::input_i64:
            push_value(
                static_cast<std::uint64_t>(read_input_i64()));
            break;
        case Op::input_f64:
            push_value(f64_bits(read_input_f64()));
            break;
        case Op::entropy_fill: {
            const auto count = pop();
            const auto pointer = pop();
            auto bytes = byte_span(pointer, count);
            std::random_device entropy;
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                auto value = entropy();
                for (std::size_t byte = 0;
                     byte < sizeof(value) && offset < bytes.size(); ++byte) {
                    bytes[offset++] = static_cast<std::uint8_t>(value);
                    value >>= 8;
                }
            }
            push_value(1);
            break;
        }
        case Op::text_length:
            push_value(text_value(text_heap, bytecode.strings, pop()).size());
            break;
        case Op::text_byte: {
            const auto index = pop();
            const auto text = pop();
            const auto& value = text_value(text_heap, bytecode.strings, text);
            if (index >= value.size()) {
                throw std::runtime_error(
                    xorstr_("Concept string byte index is out of bounds"));
            }
            push_value(static_cast<std::uint8_t>(
                value[static_cast<std::size_t>(index)]));
            break;
        }
        case Op::text_from_bytes: {
            const auto count = pop();
            const auto pointer = pop();
            const auto bytes = byte_span(pointer, count);
            if (bytes.empty()) {
                text_heap.emplace_back();
            } else {
                text_heap.emplace_back(
                    reinterpret_cast<const char*>(bytes.data()), bytes.size());
            }
            push_value(text_heap.size() - 1);
            break;
        }
        case Op::system_verify_x509: {
            const auto count = pop();
            const auto pointer = pop();
            const auto host = pop();
            const auto bytes = byte_span(pointer, count);
            push_value(verify_system_x509_chain(
                                text_value(text_heap, bytecode.strings, host), bytes)
                                ? 1
                                : 0);
            break;
        }
        case Op::print:
        case Op::println: {
            const auto type = operands.read_type();
            print_value(type, pop(), text_heap, bytecode.strings, op == Op::println);
            push_value(0);
            break;
        }
        case Op::native_call: {
            const auto argument_count = pop();
            if (argument_count > 4) {
                throw std::runtime_error(
                    xorstr_("Concept native call has too many arguments"));
            }
            std::array<std::uintptr_t, 4> arguments{};
            for (std::size_t index =
                     static_cast<std::size_t>(argument_count);
                 index != 0; --index) {
                const auto kind = pop();
                const auto value = pop();
                if (kind == 0) {
                    arguments[index - 1] =
                        static_cast<std::uintptr_t>(value);
                } else if (kind == 1) {
                    arguments[index - 1] =
                        reinterpret_cast<std::uintptr_t>(
                            text_value(text_heap, bytecode.strings, value).c_str());
                } else if (kind == 2) {
                    arguments[index - 1] = native_pointer_address(value);
                } else {
                    throw std::runtime_error(xorstr_(
                        "Concept native call has an invalid argument type"));
                }
            }
            const auto symbol = pop();
            const auto module = pop();
            push_value(invoke_native_call(
                text_value(text_heap, bytecode.strings, module),
                text_value(text_heap, bytecode.strings, symbol), arguments,
                static_cast<std::size_t>(argument_count)));
            break;
        }
        case Op::socket_open:
            push_value(socket_bits(open_tcp_socket()));
            break;
        case Op::socket_connect:
        case Op::socket_bind: {
            const auto port = static_cast<std::uint16_t>(pop());
            const auto host = pop();
            const auto handle = native_socket(pop());
            const auto& host_text = text_value(text_heap, bytecode.strings, host);
            const bool success =
                op == Op::socket_connect
                    ? connect_tcp_socket(handle, host_text, port)
                    : bind_tcp_socket(handle, host_text, port);
            push_value(success ? 1 : 0);
            break;
        }
        case Op::socket_listen: {
            const auto backlog = static_cast<int>(
                signed_value(ValueType::i32, pop()));
            const auto handle = native_socket(pop());
            push_value(listen_tcp_socket(handle, backlog) ? 1 : 0);
            break;
        }
        case Op::socket_accept:
            push_value(socket_bits(accept_tcp_socket(native_socket(pop()))));
            break;
        case Op::socket_send: {
            const auto text = pop();
            const auto handle = native_socket(pop());
            push_value(static_cast<std::uint64_t>(
                send_tcp_text(handle, text_value(text_heap, bytecode.strings, text))));
            break;
        }
        case Op::socket_receive:
            text_heap.push_back(receive_tcp_text(native_socket(pop())));
            push_value(text_heap.size() - 1);
            break;
        case Op::socket_send_bytes: {
            const auto count = pop();
            const auto pointer = pop();
            const auto handle = native_socket(pop());
            push_value(static_cast<std::uint64_t>(
                send_tcp_bytes(handle, byte_span(pointer, count))));
            break;
        }
        case Op::socket_receive_bytes: {
            const auto count = pop();
            const auto pointer = pop();
            const auto handle = native_socket(pop());
            push_value(static_cast<std::uint64_t>(
                receive_tcp_bytes(handle, byte_span(pointer, count))));
            break;
        }
        case Op::socket_close: {
            const auto handle = native_socket(pop());
            push_value(handle != invalid_socket &&
                                    close_native_socket(handle)
                                ? 1
                                : 0);
            break;
        }
        case Op::set_complexity: {
            frames.back().complexity = operands.read_u8();
            // Establish the masked representation for this frame's locals. The
            // arguments were written in the clear before the frame began
            // executing, so mask them now that the frame is known obfuscated.
            if (frames.back().complexity != 0) {
                auto& frame = frames.back();
                for (std::size_t index = 0; index < frame.locals.size();
                     ++index) {
                    frame.locals[index] ^= local_raw_mask(frame.id, index);
                }
            }
            break;
        }
        case Op::return_value: {
            const auto result = pop();
            const auto stack_base = frames.back().stack_base;
            const auto return_ip = frames.back().return_ip;
            const auto constructor_result =
                frames.back().constructor_result;
            stack.resize(stack_base);
            frames.pop_back();
            if (frames.empty()) {
                return exit_value(bytecode.entry_type, result);
            }
            push_value(constructor_result == 0 ? result
                                                    : constructor_result);
            ip = return_ip;
            break;
        }
        default:
            throw std::runtime_error(
                xorstr_("Concept VM encountered an invalid opcode"));
        }
    }
}

} // namespace cpt
