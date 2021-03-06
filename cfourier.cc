#include <vector>
#include <cassert>
#include <cstddef>
#include <cmath>
#include <complex>
#include <map>
#include <iostream>
#include <fstream>
#include <random>
#include <boost/noncopyable.hpp>
#include <CL/opencl.h>
#include <iomanip>

typedef float Float;
typedef std::complex<Float> Complex;
typedef std::vector<Complex> Signal;

static const Float eps = 0.01;
static const Complex i(0, 1);

static bool operator==(cl_float2 a, cl_float2 b) __attribute__((unused));
static bool operator==(cl_float2 a, cl_float2 b)
{
    return a.s[0] == b.s[0] && a.s[1] == b.s[1];
}

static size_t reverse_bits(size_t n, size_t max)
{
    size_t result = 0;

    for (size_t i = 1; i != max; i <<= 1) {
        result <<= 1;
        result |= (n & 1);
        n >>= 1;
    }

    return result;
}

static Signal operator-(Signal const& a, Signal const& b)
{
    assert(a.size() == b.size());
    Signal result(a.size());

    for (size_t i = 0; i != result.size(); ++i) {
        result[i] = a[i] - b[i];
    }

    return result;
}

static Complex dot(Complex const& a, Complex const& b)
{
    return a*std::conj(b);
}

static Complex dot(Signal const& a, Signal const& b)
{
    assert(a.size() == b.size());

    Complex result = 0;
    for (size_t i = 0; i != a.size(); ++i) {
        result += dot(a[i], b[i]);
    }
    return result;
}

static Signal dft(Signal const& signal)
{
    Signal result(signal.size());

    for (size_t k = 0; k != result.size(); ++k) {
        Complex c(0, 0);

        for (size_t n = 0; n != signal.size(); ++n) {
            c += signal[n]*exp(-i*(Float) 2.0*(Float) M_PI*(Float) k*(Float) n/(Float) signal.size());
        }

        result[k] = c;
    }

    return result;
}

static Signal idft(Signal const& spectrum)
{
    Signal result(spectrum.size());

    for (size_t n = 0; n != result.size(); ++n) {
        Complex c(0, 0);

        for (size_t k = 0; k != spectrum.size(); ++k) {
            c += spectrum[k]*exp(i*(Float) 2.0*(Float) M_PI*(Float) k*(Float) n/(Float) spectrum.size());
        }

        result[n] = c/(Float) spectrum.size();
    }

    return result;
}

static void print_signal(std::string name, Signal const& signal) __attribute__((unused));
static void print_signal(std::string name, Signal const& signal)
{
    std::cout << name << "=";
    for (size_t i = 0; i != signal.size(); ++i) {
        std::cout << signal[i] << ",";
    }
    std::cout << "\n";
}

static inline Complex W(int k, int N)
{
    return exp(-i*(Float) 2.0*(Float) M_PI*(Float) k/(Float) N);
}

static inline Complex Q(int n, int N)
{
    return exp(i*(Float) 2.0*(Float) M_PI*(Float) n/(Float) N);
}

static void fft_init(Complex* dst, Complex const* src, size_t N)
{
    for (size_t i = 0; i != N; ++i) {
        dst[i] = src[reverse_bits(i, N)];
    }
}

static void fft_step_spectrum(Complex* spectrum, size_t spectrumSize)
{
    for (size_t i = 0; i != spectrumSize/2; ++i) {
        size_t sample1 = i;
        size_t sample2 = i + spectrumSize/2;

        Complex even = spectrum[sample1];
        Complex odd = spectrum[sample2];

        spectrum[sample1] = even + W(sample1, spectrumSize)*odd;
        spectrum[sample2] = even + W(sample2, spectrumSize)*odd;
    }
}

static void fft_step(Complex* spectrum, size_t transform_count, size_t sample_count)
{
    for (size_t transform = 0; transform != transform_count; ++transform) {
        fft_step_spectrum(&spectrum[transform*sample_count], sample_count);
    }
}

static Signal fft(Signal const& signal)
{
    size_t const N = signal.size();
    Signal result(N);

    fft_init(&result[0], &signal[0], N);

    size_t transform_count = N/2;
    while (transform_count >= 1) {
        size_t sample_count = N/transform_count;

        fft_step(&result[0], transform_count, sample_count);

        transform_count >>= 1;
    }

    return result;
}

static void ifft_step(Complex* spectrum, size_t spectrumSize)
{
    for (size_t i = 0; i != spectrumSize/2; ++i) {
        size_t sample1 = i;
        size_t sample2 = i + spectrumSize/2;

        Complex even = spectrum[sample1];
        Complex odd = spectrum[sample2];

        spectrum[sample1] = (Float) 0.5*(even + Q(sample1, spectrumSize)*odd);
        spectrum[sample2] = (Float) 0.5*(even + Q(sample2, spectrumSize)*odd);
    }
}

static Signal ifft(Signal const& spectrum)
{
    size_t const N = spectrum.size();
    Signal result(N);

    for (size_t i = 0; i != N; ++i) {
        result[i] = spectrum[reverse_bits(i, N)];
    }

    size_t sample_count = 2;
    size_t transform_count = N/sample_count;
    while (sample_count <= N) {
        assert(transform_count*sample_count == N);

        for (size_t transform = 0; transform != transform_count; ++transform) {
            ifft_step(&result[transform*sample_count], sample_count);
        }

        transform_count >>= 1;
        sample_count <<= 1;
    }

    return result;
}

// RMS of error signal.
static Float error(Signal const& a, Signal const& b)
{
    auto e = a - b;
    return sqrt(std::real(dot(e, e))/e.size());
}

static Float prop_inverse_dft(Signal const& test_signal)
{
    return error(test_signal, idft(dft(test_signal)));
}

static Float prop_inverse_fft(Signal const& test_signal)
{
    return error(test_signal, ifft(fft(test_signal)));
}

static Float prop_dft_equal_fft(Signal const& test_signal)
{
    return error(dft(test_signal), fft(test_signal));
}

static Float prop_idft_equal_ifft(Signal const& test_signal)
{
    return error(idft(test_signal), ifft(test_signal));
}

static Float prop_fft_is_decomposed_dft(Signal const& test_signal)
{
    Signal even_samples;
    Signal odd_samples;

    // Partition even and odd samples.
    for (size_t i = 0; i != test_signal.size()/2; ++i) {
        even_samples.push_back(test_signal[2*i]);
        odd_samples.push_back(test_signal[2*i + 1]);
    }

    Signal even_spectrum = dft(even_samples);
    Signal odd_spectrum = dft(odd_samples);

    Signal intermediate_spectrum;
    intermediate_spectrum.insert(intermediate_spectrum.end(), even_spectrum.begin(), even_spectrum.end());
    intermediate_spectrum.insert(intermediate_spectrum.end(), odd_spectrum.begin(), odd_spectrum.end());

    fft_step_spectrum(&intermediate_spectrum[0], intermediate_spectrum.size());

    return error(dft(test_signal), intermediate_spectrum);
}

static bool prop_reverse_bits(size_t n, size_t max, size_t correct)
{
    return reverse_bits(n, max) == correct;
}

#define TEST_RESIDUE(signal) test_residue(#signal, signal)

static void test_residue(const char* test_name, Float residue)
{
    if (residue < eps) {
        std::cout << test_name << ": PASS: residue=" << residue << std::endl;
    }
    else {
        std::cout << test_name << ": FAIL: residue=" << residue << std::endl;
    }
}

#define TEST(prop) test(#prop, prop)

static void test(const char* test_name, bool result)
{
    if (result) {
        std::cout << test_name << ": PASS" << std::endl;
    }
    else {
        std::cout << test_name << ": FAIL" << std::endl;
    }
}

static Signal random_signal(size_t size)
{
    static std::default_random_engine generator(0);
    std::uniform_real_distribution<Float> distribution(0.0, 1.0);

    Signal result(size);
    for (size_t i = 0; i != result.size(); ++i) {
        result[i] = Complex(distribution(generator), distribution(generator));
    }
    return result;
}

static void notify(char const* errinfo, void const* private_info, size_t cb, void* user_data) __attribute__((unused));
static void notify(char const* errinfo, void const* private_info, size_t cb, void* user_data)
{
    std::cout << "OpenCL error: " << errinfo << "\n";
}

static void fatal(std::string const& msg) __attribute__((noreturn));
static void fatal(std::string const& msg)
{
    std::cout << "ERROR: " << msg << "\n";
    abort();
}

static std::string error_code_to_string(cl_int ec) __attribute__((unused));
static std::string error_code_to_string(cl_int ec)
{
    switch (ec) {
        case CL_INVALID_COMMAND_QUEUE: return "CL_INVALID_COMMAND_QUEUE";
        case CL_INVALID_CONTEXT: return "CL_INVALID_CONTEXT";
        case CL_INVALID_DEVICE: return "CL_INVALID_DEVICE";
        case CL_INVALID_EVENT_WAIT_LIST: return "CL_INVALID_EVENT_WAIT_LIST";
        case CL_INVALID_GLOBAL_OFFSET: return "CL_INVALID_GLOBAL_OFFSET";
        case CL_INVALID_GLOBAL_WORK_SIZE: return "CL_INVALID_GLOBAL_WORK_SIZE";
        case CL_INVALID_IMAGE_SIZE: return "CL_INVALID_IMAGE_SIZE";
        case CL_INVALID_KERNEL_ARGS: return "CL_INVALID_KERNEL_ARGS";
        case CL_INVALID_KERNEL: return "CL_INVALID_KERNEL";
        case CL_INVALID_PROGRAM_EXECUTABLE: return "CL_INVALID_PROGRAM_EXECUTABLE";
        case CL_INVALID_VALUE: return "CL_INVALID_VALUE";
        case CL_INVALID_WORK_DIMENSION: return "CL_INVALID_WORK_DIMENSION";
        case CL_INVALID_WORK_GROUP_SIZE: return "CL_INVALID_WORK_GROUP_SIZE";
        case CL_INVALID_WORK_ITEM_SIZE: return "CL_INVALID_WORK_ITEM_SIZE";
        case CL_MEM_OBJECT_ALLOCATION_FAILURE: return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
        case CL_MISALIGNED_SUB_BUFFER_OFFSET: return "CL_MISALIGNED_SUB_BUFFER_OFFSET";
        case CL_OUT_OF_HOST_MEMORY: return "CL_OUT_OF_HOST_MEMORY";
        case CL_OUT_OF_RESOURCES: return "CL_OUT_OF_RESOURCES";
        case CL_SUCCESS: return "CL_SUCCESS";
        default: return "<UNKNOWN>";
    }
}

static std::map<cl_platform_id, std::pair<std::string, std::string>> get_platforms()
{
    std::map<cl_platform_id, std::pair<std::string, std::string>> result;

    std::vector<cl_platform_id> platforms(16);
    cl_uint platform_count;
    if (clGetPlatformIDs(platforms.size(), &platforms[0], &platform_count) != CL_SUCCESS) {
        fatal("Could not get platform IDs.");
    }
    platforms.resize(platform_count);

    for (auto& platform_id : platforms) {
        std::vector<char> platformName(64);
        if (clGetPlatformInfo(
                    platform_id,
                    CL_PLATFORM_NAME,
                    platformName.size(),
                    &platformName[0],
                    NULL) != CL_SUCCESS) {
            fatal("Could not get platform name.");
        }

        std::vector<char> platformVersion(64);
        if (clGetPlatformInfo(
                    platform_id,
                    CL_PLATFORM_VERSION,
                    platformVersion.size(),
                    &platformVersion[0],
                    NULL) != CL_SUCCESS) {
            fatal("Could not get platform version.");
        }

        result[platform_id] = std::make_pair(&platformName[0], &platformVersion[0]);
    }

    return result;
}

static std::map<cl_device_id, std::string> get_devices(cl_platform_id platform)
{
    std::map<cl_device_id, std::string> result;

    std::vector<cl_device_id> devices(16);
    cl_uint device_count;
    if (clGetDeviceIDs(
                platform,
                CL_DEVICE_TYPE_ALL,
                devices.size(),
                &devices[0],
                &device_count) != CL_SUCCESS) {
        fatal("Could not get device IDs.");
    }
    devices.resize(device_count);

    for (auto& device_id : devices) {
        std::vector<char> name(64);
        if (clGetDeviceInfo(
                device_id,
                CL_DEVICE_NAME,
                name.size(),
                &name[0],
                NULL) != CL_SUCCESS) {
            fatal("Could not get device name.");
        }

        result[device_id] = &name[0];
    }

    return result;
}

static void print_platforms()
{
    for (auto& platform : get_platforms()) {
        std::cout
            << platform.first
            << ": name='" << platform.second.first
            << "', version='" << platform.second.second
            << "'\n";

        for (auto& device : get_devices(platform.first)) {
            std::cout
                << "  "
                << device.first
                << ": name='" << device.second
                << "'\n";
        }
    }
}

static cl_platform_id get_platform(std::string const& name)
{
    for (auto& platform : get_platforms()) {
        if (platform.second.first == name) return platform.first;
    }

    fatal("Could not find platform.");
}

static cl_device_id get_device(cl_platform_id platform, std::string const& name)
{
    for (auto& device : get_devices(platform)) {
        if (device.second == name) return device.first;
    }

    fatal("Could not find device.");
}

static std::vector<char> read_program(std::string const& name)
{
    std::fstream f(name);
    if (!f) {
        fatal("Could not read file: " + name);
    }

    f.seekg(0, std::ios_base::end);
    size_t size = f.tellg();
    f.seekg(0, std::ios_base::beg);

    std::vector<char> result(size);

    f.read(&result[0], result.size());
    result.push_back(0);
    return result;
}

void convert(cl_float2* dst, Complex const* src, size_t count)
{
    for (size_t i = 0; i != count; ++i) {
        dst[i].s[0] = std::real(src[i]);
        dst[i].s[1] = std::imag(src[i]);
    }
}

void convert(Complex* dst, cl_float2 const* src, size_t count)
{
    for (size_t i = 0; i != count; ++i) {
        dst[i] = Complex(src[i].s[0], src[i].s[1]);
    }
}

void convert(Complex* dst, std::vector<cl_float2> const& src)
{
    convert(dst, &src[0], src.size());
}

std::vector<cl_float2> to_float2_vector(Complex const* vec, size_t count)
{
    std::vector<cl_float2> result(count);

    convert(&result[0], vec, count);

    return result;
}

std::vector<cl_float2> to_float2_vector(std::vector<Complex> const& vec)
{
    return to_float2_vector(&vec[0], vec.size());
}

class Fourier : private boost::noncopyable
{
public:
    explicit Fourier(size_t sample_power)
        : m_sample_power(sample_power)
    {
        print_platforms();

        cl_platform_id platform = get_platform("NVIDIA CUDA");
        cl_device_id device = get_device(platform, "GeForce GTX 970");

        cl_context_properties properties[] = { CL_CONTEXT_PLATFORM, (cl_context_properties) platform, 0 };
        m_context = clCreateContext(properties, 1, &device, notify, NULL, NULL);
        if (0 == m_context) fatal("Could not create contex.");

        m_queue = clCreateCommandQueue(m_context, device, 0, NULL);
        if (0 == m_queue) fatal("Could not create command queue.");

        std::vector<std::vector<char>> sources;
        sources.push_back(read_program("fourier.cl"));

        std::vector<char const*> sources_raw;
        for (auto& source : sources) {
            sources_raw.push_back(&source[0]);
        }

        m_program = clCreateProgramWithSource(
                m_context,
                sources_raw.size(),
                &sources_raw[0],
                NULL,
                NULL);
        if (m_program == NULL) fatal("Could not create program.");

        if (clBuildProgram(
                    m_program,
                    1,
                    &device,
                    "",
                    NULL,
                    NULL) != CL_SUCCESS) {
            std::vector<char> build_log(1024);
            size_t build_log_size;
            if (clGetProgramBuildInfo(
                        m_program,
                        device,
                        CL_PROGRAM_BUILD_LOG,
                        build_log.size(),
                        &build_log[0],
                        &build_log_size)
                    != CL_SUCCESS) {
                fatal("Could not get program build info.");
            }
            build_log.resize(build_log_size);

            std::cout << &build_log[0];

            fatal("Could not build program.");
        }

        m_init_kernel = clCreateKernel(m_program, "fft_init", NULL);
        if (m_init_kernel == NULL) fatal("Could not create init kernel.");

        m_step_kernel = clCreateKernel(m_program, "fft_step", NULL);
        if (m_step_kernel == NULL) fatal("Could not create step kernel.");

        m_x_mem = clCreateBuffer(
                m_context,
                CL_MEM_READ_ONLY,
                byte_count(),
                NULL,
                NULL);
        if (m_x_mem == NULL) fatal("Could not create X buffer.");

        m_y1_mem = clCreateBuffer(
                m_context,
                CL_MEM_WRITE_ONLY,
                byte_count(),
                NULL,
                NULL);
        if (m_y1_mem == NULL) fatal("Could not create Y1 buffer.");

        m_y2_mem = clCreateBuffer(
                m_context,
                CL_MEM_WRITE_ONLY,
                byte_count(),
                NULL,
                NULL);
        if (m_y2_mem == NULL) fatal("Could not create Y2 buffer.");
    }

    ~Fourier()
    {
        if (clReleaseMemObject(m_y2_mem) != CL_SUCCESS) fatal("Could not release Y2 buffer.");
        if (clReleaseMemObject(m_y1_mem) != CL_SUCCESS) fatal("Could not release Y1 buffer.");
        if (clReleaseMemObject(m_x_mem) != CL_SUCCESS) fatal("Could not release X buffer.");
        if (clReleaseKernel(m_step_kernel) != CL_SUCCESS) fatal("Could not release step kernel.");
        if (clReleaseKernel(m_init_kernel) != CL_SUCCESS) fatal("Could not release init kernel.");
        if (clReleaseProgram(m_program) != CL_SUCCESS) fatal("Could not release program");
        if (clUnloadCompiler() != CL_SUCCESS) fatal("Could not unload compiler.");
        if (clReleaseCommandQueue(m_queue) != CL_SUCCESS) fatal("Could not release command queue.");
        if (clReleaseContext(m_context) != CL_SUCCESS) fatal("Could not release context.");
    }

    void init(cl_mem x, cl_uint sample_power, cl_mem y)
    {
        set_arg(m_init_kernel, 0, x);
        set_arg(m_init_kernel, 1, sample_power);
        set_arg(m_init_kernel, 2, y);

        run_kernel(m_init_kernel);
    }

    void init(Complex* dst, Complex const* src)
    {
        std::vector<cl_float2> x_buffer = to_float2_vector(src, sample_count());

        load(m_x_mem, x_buffer);

        init(m_x_mem, m_sample_power, m_y1_mem);

        std::vector<cl_float2> y1_buffer(sample_count());
        store(&y1_buffer[0], m_y1_mem);

        finish();

        convert(dst, y1_buffer);
    }

    void step(cl_mem y, cl_uint B, cl_mem y_)
    {
        set_arg(m_step_kernel, 0, y);
        set_arg(m_step_kernel, 1, B);
        set_arg(m_step_kernel, 2, y_);

        run_kernel(m_step_kernel);
    }

    void step(Complex* dst, Complex const* src, size_t B)
    {
        std::vector<cl_float2> y1_buffer = to_float2_vector(src, sample_count());

        load(m_y1_mem, y1_buffer);

        step(m_y1_mem, B, m_y2_mem);

        std::vector<cl_float2> y2_buffer(sample_count());
        store(&y2_buffer[0], m_y2_mem);

        finish();

        convert(dst, y2_buffer);
    }

    void fft(Complex* spectrum, Complex const* signal)
    {
        std::vector<cl_float2> x_buffer = to_float2_vector(signal, sample_count());
        load(m_x_mem, x_buffer);

        init(m_x_mem, m_sample_power, m_y1_mem);

        cl_mem y = m_y1_mem;
        cl_mem y_ = m_y2_mem;
        cl_uint B = 1;
        while (B != sample_count()) {
            step(y, B, y_);
            std::swap(y, y_);
            B <<= 1;
        }

        std::vector<cl_float2> y_buffer(sample_count());
        store(&y_buffer[0], y);
        finish();

        convert(spectrum, y_buffer);
    }

    void flush()
    {
        if (clFlush(m_queue) != CL_SUCCESS) fatal("Could not flush.");
    }

    void finish()
    {
        if (clFinish(m_queue) != CL_SUCCESS) fatal("Could not finish.");
    }

    void run_kernel(cl_kernel kernel)
    {
        cl_int ec;

        size_t global_work_size = sample_count();
        ec = clEnqueueNDRangeKernel(
                m_queue,
                kernel,
                1,
                NULL,
                &global_work_size,
                NULL,
                0,
                NULL,
                NULL);
        if (ec != CL_SUCCESS) {
            std::cout << error_code_to_string(ec) << "\n";
            fatal("Could not enqueue kernel.");
        }
    }

    void set_arg(cl_kernel kernel, cl_uint arg_index, cl_uint arg)
    {
        if (clSetKernelArg(
                    kernel,
                    arg_index,
                    sizeof(arg),
                    &arg
                    ) != CL_SUCCESS) {
            fatal("Could not set kernel argument.");
        }
    }

    void set_arg(cl_kernel kernel, cl_uint arg_index, cl_mem arg)
    {
        if (clSetKernelArg(
                    kernel,
                    arg_index,
                    sizeof(arg),
                    &arg
                    ) != CL_SUCCESS) {
            fatal("Could not set kernel argument.");
        }
    }

    void load(cl_mem mem, std::vector<cl_float2> const& buffer)
    {
        assert(buffer.size() == sample_count());

        if (clEnqueueWriteBuffer(
                    m_queue,
                    mem,
                    CL_TRUE,
                    0,
                    byte_count(),
                    &buffer[0],
                    0,
                    NULL,
                    NULL) != CL_SUCCESS) {
            fatal("Coule not write to buffer.");
        }
    }

    void store(cl_float2* buffer, cl_mem mem)
    {
        cl_int ec;

        ec = clEnqueueReadBuffer(
                m_queue,
                mem,
                CL_TRUE,
                0,
                byte_count(),
                buffer,
                0,
                NULL,
                NULL);
        if (ec != CL_SUCCESS) {
            std::cout << error_code_to_string(ec) << "\n";
            fatal("Could not read buffer.");
        }
    }

    size_t byte_count() const
    {
        return sample_count()*sizeof(cl_float2);
    }

    size_t sample_count() const
    {
        return 1 << m_sample_power;
    }

private:
    size_t m_sample_power;
    cl_mem m_y2_mem;
    cl_mem m_y1_mem;
    cl_mem m_x_mem;
    cl_kernel m_step_kernel;
    cl_kernel m_init_kernel;
    cl_program m_program;
    cl_command_queue m_queue;
    cl_context m_context;
};

static void print_reverse_bits_table() __attribute((unused));
static void print_reverse_bits_table()
{
    std::stringstream ss;

    for (size_t row = 0; row != 16; ++row) {
        for (size_t column = 0; column != 16; ++column) {
            ss << std::hex << std::setw(2) << std::setfill('0');
            ss << reverse_bits(row*16+column, 256) << " ";
        }
        ss << "\n";
    }

    std::cout << ss.str();
}

static Float prop_fftcl_init_equals_fft_init(Fourier& fourier, Signal const& signal)
{
    assert(fourier.sample_count() == signal.size());

    Signal expected(signal.size());
    fft_init(&expected[0], &signal[0], expected.size());

    Signal actual(signal.size());
    fourier.init(&actual[0], &signal[0]);

    return error(expected, actual);
}

static Float prop_fftcl_step_equals_fft_step(Fourier& fourier, Signal const& signal)
{
    assert(fourier.sample_count() == signal.size());

    size_t B = 128;
    size_t transform_count = signal.size()/B;
    assert(B >= 1);

    Signal expected = signal;
    fft_step(&expected[0], transform_count/2, 2*B);

    Signal actual(signal.size());
    fourier.step(&actual[0], &signal[0], B);

    return error(expected, actual);
}

static Float prop_fftcl_equals_fft(Fourier& fourier, Signal const& signal)
{
    assert(fourier.sample_count() == signal.size());

    Signal expected = fft(signal);

    Signal actual(signal.size());
    fourier.fft(&actual[0], &signal[0]);

    return error(expected, actual);
}

int main()
{
    Fourier fourier(10);

    TEST_RESIDUE(prop_inverse_dft(Signal(1024, 1)));
    TEST_RESIDUE(prop_inverse_dft(random_signal(1024)));
    TEST_RESIDUE(prop_inverse_fft(Signal(2, 1)));
    TEST_RESIDUE(prop_inverse_fft(Signal(1024, 1)));
    TEST_RESIDUE(prop_inverse_fft(random_signal(1024)));
    TEST_RESIDUE(prop_dft_equal_fft(Signal(1024, 1)));
    TEST_RESIDUE(prop_dft_equal_fft(random_signal(4)));
    TEST_RESIDUE(prop_dft_equal_fft(random_signal(1024)));
    TEST_RESIDUE(prop_dft_equal_fft(Signal{7,6,5,4,3,2,i,0}));
    TEST_RESIDUE(prop_idft_equal_ifft(random_signal(1024)));
    TEST_RESIDUE(prop_idft_equal_ifft(Signal{1.1,i,2.1,3}));
    TEST_RESIDUE(prop_fft_is_decomposed_dft(Signal{7,6,5,4,3,2,i,0}));
    TEST_RESIDUE(prop_fft_is_decomposed_dft(random_signal(1024)));
    TEST(prop_reverse_bits(0xAA, 0x100, 0x55));
    TEST(prop_reverse_bits(0xA5, 0x100, 0xA5));
    TEST_RESIDUE(prop_fftcl_init_equals_fft_init(fourier, random_signal(1024)));
    TEST_RESIDUE(prop_fftcl_step_equals_fft_step(fourier, random_signal(1024)));
    TEST_RESIDUE(prop_fftcl_equals_fft(fourier, random_signal(1024)));

    return 0;
}
