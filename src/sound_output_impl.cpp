#include "bass/bass.h"
#include "voice_exception.hpp"
#include "sound_output_impl.hpp"

#include "stream_impl.hpp"

kvoice::sound_output_impl::sound_output_impl(std::string_view device_name, std::uint32_t sample_rate)
    : sampling_rate(sample_rate), requests_queue(16) {
    using namespace std::string_literals;

    std::mutex              condvar_mtx{};
    std::condition_variable output_initialization{};
    std::exception_ptr      initialization_exception{nullptr};
    output_alive = true;
    output_thread = std::thread([this, sample_rate, device_name, &output_initialization, &initialization_exception]() {
        int init_idx = -1;
        if (!device_name.empty()) {
            BASS_DEVICEINFO info;
            for (auto i = 1; BASS_GetDeviceInfo(i, &info); i++) {
                if (info.flags & BASS_DEVICE_ENABLED) {
                    if (device_name == info.name) {
                        init_idx = i;
                        break;
                    }
                }
            }

        }
        auto result = BASS_Init(init_idx, sample_rate, BASS_DEVICE_MONO | BASS_DEVICE_3D, nullptr, nullptr);
        if (!result) {
            initialization_exception = std::make_exception_ptr(voice_exception::create_formatted("Couldn't open capture device {}", device_name));
            output_initialization.notify_one();
            return;
        }

        // dummy https request to init OpenSSL(not thread safe in basslib)
        auto temp_handle = BASS_StreamCreateURL("https://www.google.com", 0, 0, NULL, 0);

        BASS_StreamFree(temp_handle);

        output_initialization.notify_one();

        while (output_alive.load()) {
            BASS_SetConfig(BASS_CONFIG_GVOL_STREAM, static_cast<unsigned>(output_gain.load() * 10000));
            {
                std::lock_guard lock(spatial_mtx);

                auto vec_convert = [](kvoice::vector vec) {
                    return BASS_3DVECTOR{ vec.x, vec.y, vec.z };
                };

                BASS_3DVECTOR pos = vec_convert(listener_pos);
                BASS_3DVECTOR vel = vec_convert(listener_vel);
                BASS_3DVECTOR front = vec_convert(listener_front);
                BASS_3DVECTOR up = vec_convert(listener_up);

                BASS_Set3DPosition(&pos, &vel, &front, &up);
                BASS_Apply3D();
            }
            if (device_need_update.load()) {
                BASS_DEVICEINFO info;
                for (auto i = 1u; BASS_GetDeviceInfo(i, &info); i++) {
                    if (info.flags & BASS_DEVICE_ENABLED) {
                        if (this->device_name == info.name) {
                            if (BASS_SetDevice(i)) {
                                
                            } else {
                                BASS_SetDevice(-1);
                            }
                        }
                    }
                }
                BASS_SetDevice(-1);
                device_need_update.store(false);
            }
            requests_queue.consume_all([this](const request_stream_message& msg) {
                if (msg.params.has_value()) {
                    auto& params = *msg.params;
                    msg.on_creation_callback(std::make_unique<stream_impl>(this, params.url, params.file_offset, this->sampling_rate));
                }
                else {
                    msg.on_creation_callback(std::make_unique<stream_impl>(this, this->sampling_rate));
                }
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        BASS_Free();
    });

    std::unique_lock lock{ condvar_mtx };
    output_initialization.wait(lock);
    if (initialization_exception != nullptr) {
        std::rethrow_exception(initialization_exception);
    }
}

kvoice::sound_output_impl::~sound_output_impl() {
    output_alive = false;
    output_thread.join();
}

void kvoice::sound_output_impl::set_my_position(vector pos) noexcept {
    std::lock_guard lock{ spatial_mtx };
    listener_pos = pos;
}

void kvoice::sound_output_impl::set_my_velocity(vector vel) noexcept {
    std::lock_guard lock{ spatial_mtx };
    listener_vel = vel;
}

void kvoice::sound_output_impl::set_my_orientation_up(vector up) noexcept {
    std::lock_guard lock{ spatial_mtx };
    listener_up = up;
}

void kvoice::sound_output_impl::set_my_orientation_front(vector front) noexcept {
    std::lock_guard lock{ spatial_mtx };
    listener_front = front;
}

void kvoice::sound_output_impl::set_gain(float gain) noexcept {
    output_gain.store(gain);
}

void kvoice::sound_output_impl::change_device(std::string_view device_name) {
    if (device_need_update.load()) return;
    this->device_name = device_name;
    device_need_update.store(true);
}

void kvoice::sound_output_impl::create_stream(on_create_callback cb) {
    requests_queue.push(request_stream_message{std::nullopt, cb});
}

void kvoice::sound_output_impl::create_stream(on_create_callback cb, 
    std::string_view url, std::uint32_t file_offset) {
    requests_queue.push(request_stream_message{std::make_optional(online_stream_parameters{ std::string{ url }, file_offset }), std::move(cb)});
}
