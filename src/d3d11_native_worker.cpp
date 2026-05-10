#include "d3d11_native_worker.h"

#include "d3d11_native_slot_pool.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <QDebug>
#include <QtMath>

#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
QString hresultToString(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<unsigned int>(hr), 0, 16);
}

bool isAcquireTimeout(HRESULT hr)
{
    return hr == WAIT_TIMEOUT || hr == DXGI_ERROR_WAIT_TIMEOUT;
}

void logWorkerMessage(const QString &message)
{
    if (!message.isEmpty()) {
        qInfo().noquote() << "[D3D11NativeWorker]" << message;
    }
}

QSize sanitizedSize(const QSize &size)
{
    return QSize(qMax(1, size.width()), qMax(1, size.height()));
}

UINT alignConstantBufferByteWidth(UINT size)
{
    return (size + 15u) & ~15u;
}

bool createD3D11Device(ComPtr<ID3D11Device> *device,
                       ComPtr<ID3D11DeviceContext> *context,
                       QString *error)
{
    if (device == nullptr || context == nullptr) {
        if (error) {
            *error = QStringLiteral("createD3D11Device prerequisites are incomplete.");
        }
        return false;
    }

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        kLevels,
        ARRAYSIZE(kLevels),
        D3D11_SDK_VERSION,
        device->ReleaseAndGetAddressOf(),
        &featureLevel,
        context->ReleaseAndGetAddressOf());

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            kLevels,
            ARRAYSIZE(kLevels),
            D3D11_SDK_VERSION,
            device->ReleaseAndGetAddressOf(),
            &featureLevel,
            context->ReleaseAndGetAddressOf());
    }

    if (FAILED(hr) || !(*device) || !(*context)) {
        if (error) {
            *error = QStringLiteral("D3D11CreateDevice failed: %1").arg(hresultToString(hr));
        }
        return false;
    }

    return true;
}

HRESULT compileShader(const char *source,
                      const char *entryPoint,
                      const char *target,
                      ComPtr<ID3DBlob> *blob,
                      QString *error)
{
    if (blob == nullptr) {
        return E_INVALIDARG;
    }

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    const HRESULT hr = D3DCompile(source,
                                  strlen(source),
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  entryPoint,
                                  target,
                                  flags,
                                  0,
                                  blob->ReleaseAndGetAddressOf(),
                                  errorBlob.GetAddressOf());
    if (FAILED(hr) && error) {
        const QString compilerText = errorBlob
            ? QString::fromLocal8Bit(static_cast<const char *>(errorBlob->GetBufferPointer()), int(errorBlob->GetBufferSize()))
            : QStringLiteral("<no compiler diagnostics>");
        *error = QStringLiteral("D3DCompile(%1, %2) failed: %3\n%4")
                     .arg(QString::fromLatin1(entryPoint),
                          QString::fromLatin1(target),
                          hresultToString(hr),
                          compilerText);
    }
    return hr;
}
} // namespace

struct D3D11NativeWorker::Impl final
{
    struct SlotResources final
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<IDXGIKeyedMutex> keyedMutex;
        quintptr sharedHandle = 0;
        QSize size;
        quint64 generation = 0;
    };

    struct Constants final
    {
        float brightness = 0.0f;
        float contrast = 1.0f;
        float zoom = 1.0f;
        float panX = 0.0f;
        float panY = 0.0f;
        float rotationRadians = 0.0f;
        float flipX = 1.0f;
        float flipY = 1.0f;
        float timeValue = 0.0f;
        unsigned int heavyPassCount = 0;
        float outputWidth = 1.0f;
        float outputHeight = 1.0f;
        float pad0 = 0.0f;
        float pad1 = 0.0f;
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11Buffer> constantBuffer;
    ComPtr<ID3D11SamplerState> samplerState;
    ComPtr<ID3D11ShaderResourceView> sourceTextureView;
    QVector<SlotResources> slotResources;
    QStringList imagePaths;
    int currentImageIndex = -1;
    QImage currentImage;

    bool initialize(int slotCount, QString *error)
    {
        if (!createD3D11Device(&device, &context, error)) {
            return false;
        }

        static const char *kVertexShader = R"(
struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut main(uint vertexId : SV_VertexID)
{
    float2 positions[3];
    positions[0] = float2(-1.0, -1.0);
    positions[1] = float2(-1.0, 3.0);
    positions[2] = float2(3.0, -1.0);

    VSOut output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = float2(output.position.x * 0.5 + 0.5, 0.5 - output.position.y * 0.5);
    return output;
}
)";

        static const char *kPixelShader = R"(
cbuffer RenderConstants : register(b0)
{
    float brightness;
    float contrast;
    float zoom;
    float panX;
    float panY;
    float rotationRadians;
    float flipX;
    float flipY;
    float timeValue;
    uint heavyPassCount;
    float outputWidth;
    float outputHeight;
    float pad0;
    float pad1;
};

Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(VSOut input) : SV_TARGET
{
    float2 centered = input.uv - float2(0.5, 0.5);
    uint sourceWidthUint = 1;
    uint sourceHeightUint = 1;
    sourceTexture.GetDimensions(sourceWidthUint, sourceHeightUint);

    float sourceWidth = max(float(sourceWidthUint), 1.0);
    float sourceHeight = max(float(sourceHeightUint), 1.0);
    float safeOutputWidth = max(outputWidth, 1.0);
    float safeOutputHeight = max(outputHeight, 1.0);
    float sourceAspect = sourceWidth / sourceHeight;
    float outputAspect = safeOutputWidth / safeOutputHeight;

    float2 fitHalfExtent = float2(0.5, 0.5);
    if (sourceAspect > outputAspect) {
        fitHalfExtent.y *= outputAspect / sourceAspect;
    } else {
        fitHalfExtent.x *= sourceAspect / outputAspect;
    }

    float2 p = centered / max(fitHalfExtent * 2.0, float2(1e-5, 1e-5));
    p -= float2(panX, panY);
    float s = sin(-rotationRadians);
    float c = cos(-rotationRadians);
    p = float2(c * p.x - s * p.y, s * p.x + c * p.y);

    float safeZoom = max(zoom, 0.05);
    p /= safeZoom;
    p.x *= flipX;
    p.y *= flipY;

    float2 imageUv = p + float2(0.5, 0.5);
    float2 clampedUv = saturate(imageUv);
    float4 sampled = sourceTexture.Sample(sourceSampler, clampedUv);

    float outside = step(imageUv.x, 0.0) + step(1.0, imageUv.x) + step(imageUv.y, 0.0) + step(1.0, imageUv.y);
    float inBounds = outside > 0.0 ? 0.0 : 1.0;

    float2 grid = p * float2(6.0, 6.0);
    float checker = fmod(floor(grid.x) + floor(grid.y), 2.0);
    float rings = 0.5 + 0.5 * cos(18.0 * length(p) - timeValue * 1.3);
    float stripes = 0.5 + 0.5 * sin((p.x * 11.0 + p.y * 7.0) + timeValue);

    float3 backdrop = lerp(float3(0.05, 0.06, 0.08), float3(0.16, 0.18, 0.22), checker * 0.35 + rings * 0.15);
    backdrop += float3(stripes * 0.03, rings * 0.04, checker * 0.02);
    float3 base = lerp(backdrop, sampled.rgb, inBounds);

    float3 accum = base;
    [loop]
    for (uint i = 0; i < heavyPassCount; ++i)
    {
        float fi = float(i) + 1.0;
        float wobble = sin(fi * 0.17 + p.x * (4.0 + fi * 0.02) + timeValue)
                     * cos(fi * 0.11 + p.y * (5.0 + fi * 0.03) - timeValue * 0.7);
        accum += float3(
            wobble * 0.0025,
            sin(wobble + fi * 0.09) * 0.0018,
            cos(wobble - fi * 0.05) * 0.0015);
        p += float2(wobble * 0.0004, -wobble * 0.0003);
    }

    float3 color = accum;
    color = (color - 0.5) * contrast + (0.5 + brightness);
    return float4(saturate(color), 1.0);
}
)";

        ComPtr<ID3DBlob> vsBlob;
        if (FAILED(compileShader(kVertexShader, "main", "vs_4_0", &vsBlob, error))) {
            return false;
        }

        ComPtr<ID3DBlob> psBlob;
        if (FAILED(compileShader(kPixelShader, "main", "ps_4_0", &psBlob, error))) {
            return false;
        }

        HRESULT hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
        if (FAILED(hr) || !vertexShader) {
            if (error) {
                *error = QStringLiteral("CreateVertexShader failed: %1").arg(hresultToString(hr));
            }
            return false;
        }

        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);
        if (FAILED(hr) || !pixelShader) {
            if (error) {
                *error = QStringLiteral("CreatePixelShader failed: %1").arg(hresultToString(hr));
            }
            return false;
        }

        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufferDesc.ByteWidth = alignConstantBufferByteWidth(UINT(sizeof(Constants)));
        hr = device->CreateBuffer(&bufferDesc, nullptr, &constantBuffer);
        if (FAILED(hr) || !constantBuffer) {
            if (error) {
                *error = QStringLiteral("CreateBuffer(constantBuffer) failed: %1").arg(hresultToString(hr));
            }
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&samplerDesc, &samplerState);
        if (FAILED(hr) || !samplerState) {
            if (error) {
                *error = QStringLiteral("CreateSamplerState failed: %1").arg(hresultToString(hr));
            }
            return false;
        }

        slotResources.resize(slotCount);
        return true;
    }

    QStringList collectImages(const QString &directoryPath) const
    {
        QDir directory(directoryPath);
        const QStringList filters = {
            QStringLiteral("*.png"),
            QStringLiteral("*.jpg"),
            QStringLiteral("*.jpeg"),
            QStringLiteral("*.bmp"),
            QStringLiteral("*.webp")
        };

        QStringList result;
        const QFileInfoList entries = directory.entryInfoList(
            filters,
            QDir::Files | QDir::Readable | QDir::NoSymLinks,
            QDir::Name);
        for (const QFileInfo &entry : entries) {
            result.push_back(entry.absoluteFilePath());
        }
        return result;
    }

    QString currentDisplayName() const
    {
        if (currentImageIndex < 0 || currentImageIndex >= imagePaths.size()) {
            return {};
        }
        return QFileInfo(imagePaths[currentImageIndex]).fileName();
    }

    bool hasImage() const
    {
        return !currentImage.isNull();
    }

    bool uploadCurrentImage(QString *error)
    {
        if (!device || !context || currentImage.isNull()) {
            if (error) {
                *error = QStringLiteral("No image is available for upload.");
            }
            return false;
        }

        QImage image = currentImage.convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull()) {
            if (error) {
                *error = QStringLiteral("Current image could not be converted to RGBA8888.");
            }
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = UINT(image.width());
        desc.Height = UINT(image.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initialData = {};
        initialData.pSysMem = image.constBits();
        initialData.SysMemPitch = UINT(image.bytesPerLine());

        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = device->CreateTexture2D(&desc, &initialData, &texture);
        if (FAILED(hr) || !texture) {
            if (error) {
                *error = QStringLiteral("CreateTexture2D(source image) failed: %1").arg(hresultToString(hr));
            }
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        hr = device->CreateShaderResourceView(texture.Get(), &srvDesc, &sourceTextureView);
        if (FAILED(hr) || !sourceTextureView) {
            if (error) {
                *error = QStringLiteral("CreateShaderResourceView(source image) failed: %1").arg(hresultToString(hr));
            }
            return false;
        }

        return true;
    }

    bool loadImageDirectory(const QString &directoryPath, QString *error)
    {
        const QStringList paths = collectImages(directoryPath);
        if (paths.isEmpty()) {
            if (error) {
                *error = QStringLiteral("No supported images were found in the selected directory.");
            }
            return false;
        }

        imagePaths = paths;
        currentImageIndex = 0;
        currentImage = QImage(paths.first());
        sourceTextureView.Reset();
        if (currentImage.isNull()) {
            if (error) {
                *error = QStringLiteral("Failed to load the first image from the selected directory.");
            }
            return false;
        }

        return uploadCurrentImage(error);
    }

    bool selectRelativeImage(int delta, QString *error)
    {
        if (imagePaths.isEmpty()) {
            return false;
        }

        const int count = imagePaths.size();
        currentImageIndex = (currentImageIndex + delta + count) % count;
        currentImage = QImage(imagePaths[currentImageIndex]);
        sourceTextureView.Reset();
        if (currentImage.isNull()) {
            if (error) {
                *error = QStringLiteral("Failed to load image: %1").arg(imagePaths[currentImageIndex]);
            }
            return false;
        }

        return uploadCurrentImage(error);
    }

    bool ensureSlotResources(int slotIndex, const QSize &size, D3D11NativeSlotPool *slotPool, QString *error)
    {
        if (slotIndex < 0 || slotIndex >= slotResources.size() || slotPool == nullptr) {
            if (error) {
                *error = QStringLiteral("Slot resource prerequisites are incomplete.");
            }
            return false;
        }

        SlotResources &slot = slotResources[slotIndex];
        const QSize safeSize = sanitizedSize(size);
        if (slot.texture && slot.size == safeSize && slot.sharedHandle != 0) {
            return true;
        }

        slot.texture.Reset();
        slot.rtv.Reset();
        slot.keyedMutex.Reset();
        slot.sharedHandle = 0;
        slot.size = QSize();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = UINT(safeSize.width());
        desc.Height = UINT(safeSize.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &slot.texture);
        if (FAILED(hr) || !slot.texture) {
            if (error) {
                *error = QStringLiteral("CreateTexture2D(slot %1) failed: %2").arg(slotIndex).arg(hresultToString(hr));
            }
            return false;
        }

        hr = device->CreateRenderTargetView(slot.texture.Get(), nullptr, &slot.rtv);
        if (FAILED(hr) || !slot.rtv) {
            if (error) {
                *error = QStringLiteral("CreateRenderTargetView(slot %1) failed: %2").arg(slotIndex).arg(hresultToString(hr));
            }
            return false;
        }

        hr = slot.texture.As(&slot.keyedMutex);
        if (FAILED(hr) || !slot.keyedMutex) {
            if (error) {
                *error = QStringLiteral("Query IDXGIKeyedMutex(slot %1) failed: %2").arg(slotIndex).arg(hresultToString(hr));
            }
            return false;
        }

        ComPtr<IDXGIResource> dxgiResource;
        hr = slot.texture.As(&dxgiResource);
        if (FAILED(hr) || !dxgiResource) {
            if (error) {
                *error = QStringLiteral("Query IDXGIResource(slot %1) failed: %2").arg(slotIndex).arg(hresultToString(hr));
            }
            return false;
        }

        HANDLE sharedHandle = nullptr;
        hr = dxgiResource->GetSharedHandle(&sharedHandle);
        if (FAILED(hr) || !sharedHandle) {
            if (error) {
                *error = QStringLiteral("GetSharedHandle(slot %1) failed: %2").arg(slotIndex).arg(hresultToString(hr));
            }
            return false;
        }

        slot.sharedHandle = quintptr(sharedHandle);
        slot.size = safeSize;
        ++slot.generation;
        slotPool->updateSlot(slotIndex, slot.sharedHandle, slot.size, slot.generation);
        return true;
    }

    bool renderSlot(int slotIndex,
                    const QSize &size,
                    const ImageEffectParameters &parameters,
                    quint64 frameIndex,
                    QString *error)
    {
        if (slotIndex < 0 || slotIndex >= slotResources.size()) {
            if (error) {
                *error = QStringLiteral("Render slot index is out of range.");
            }
            return false;
        }

        SlotResources &slot = slotResources[slotIndex];
        if (!slot.keyedMutex || !slot.rtv) {
            if (error) {
                *error = QStringLiteral("Render slot resources are not initialized.");
            }
            return false;
        }

        for (;;) {
            const HRESULT acquireHr = slot.keyedMutex->AcquireSync(0, 1);
            if (SUCCEEDED(acquireHr)) {
                break;
            }
            if (isAcquireTimeout(acquireHr)) {
                QThread::msleep(1);
                continue;
            }
            if (error) {
                *error = QStringLiteral("AcquireSync(slot %1, key 0) failed: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(acquireHr));
            }
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            slot.keyedMutex->ReleaseSync(0);
            if (error) {
                *error = QStringLiteral("Map(constantBuffer) failed: %1").arg(hresultToString(hr));
            }
            return false;
        }

        Constants constants;
        constants.brightness = parameters.brightness;
        constants.contrast = parameters.contrast;
        constants.zoom = qMax(0.05f, parameters.zoom);
        constants.panX = parameters.panX * 0.65f;
        constants.panY = parameters.panY * 0.65f;
        constants.rotationRadians = qDegreesToRadians(parameters.rotationDegrees);
        constants.flipX = parameters.flipHorizontal ? -1.0f : 1.0f;
        constants.flipY = parameters.flipVertical ? -1.0f : 1.0f;
        constants.timeValue = float(frameIndex) * 0.07f;
        constants.heavyPassCount = unsigned(qBound(0, parameters.heavyGpuPassCount, 1024));
        constants.outputWidth = float(size.width());
        constants.outputHeight = float(size.height());
        memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(constantBuffer.Get(), 0);

        ID3D11RenderTargetView *rtv = slot.rtv.Get();
        const float clearColor[4] = {0.06f, 0.07f, 0.09f, 1.0f};
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, clearColor);

        D3D11_VIEWPORT viewport = {};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = float(size.width());
        viewport.Height = float(size.height());
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        ID3D11Buffer *constantBuffers[] = { constantBuffer.Get() };
        context->VSSetConstantBuffers(0, 1, constantBuffers);
        context->PSSetConstantBuffers(0, 1, constantBuffers);
        ID3D11ShaderResourceView *sourceViews[] = { sourceTextureView.Get() };
        context->PSSetShaderResources(0, 1, sourceViews);
        ID3D11SamplerState *samplers[] = { samplerState.Get() };
        context->PSSetSamplers(0, 1, samplers);
        context->Draw(3, 0);
        ID3D11ShaderResourceView *nullViews[] = { nullptr };
        context->PSSetShaderResources(0, 1, nullViews);
        context->Flush();

        hr = slot.keyedMutex->ReleaseSync(1);
        if (FAILED(hr)) {
            if (error) {
                *error = QStringLiteral("ReleaseSync(slot %1, key 1) failed: %2")
                             .arg(slotIndex)
                             .arg(hresultToString(hr));
            }
            return false;
        }

        return true;
    }
};

D3D11NativeWorker::D3D11NativeWorker(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
}

D3D11NativeWorker::~D3D11NativeWorker()
{
    shutdown();
}

bool D3D11NativeWorker::initialize(D3D11NativeSlotPool *slotPool, QSize outputSize)
{
    if (m_initialized || slotPool == nullptr) {
        return false;
    }

    m_slotPool = slotPool;
    m_outputSize = sanitizedSize(outputSize);

    QString error;
    if (!m_impl->initialize(slotPool->slotCount(), &error)) {
        emit initializationFailed(error);
        m_slotPool = nullptr;
        return false;
    }

    m_slotPool->reset();
    m_initialized = true;
    logWorkerMessage(QStringLiteral("Initialized. Producer/consumer synchronization now uses shared textures + keyed mutex."));
    return true;
}

void D3D11NativeWorker::setOutputSize(QSize size)
{
    const QSize safeSize = sanitizedSize(size);
    bool changed = false;
    {
        QMutexLocker locker(&m_stateMutex);
        if (m_outputSize != safeSize) {
            m_outputSize = safeSize;
            changed = true;
        }
    }
    if (changed) {
        scheduleRender(0);
    }
}

void D3D11NativeWorker::setEffectParameters(const ImageEffectParameters &parameters)
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_effectParameters = parameters;
    }
    scheduleRender(0);
}

void D3D11NativeWorker::loadImageDirectory(const QString &directoryPath)
{
    if (!m_initialized) {
        emit imageDirectoryLoadFinished(false,
                                        QStringLiteral("D3D11 native worker is not initialized."),
                                        -1,
                                        0,
                                        {},
                                        {});
        return;
    }

    logWorkerMessage(QStringLiteral("Loading image directory: %1").arg(directoryPath));
    QString error;
    const bool loaded = m_impl->loadImageDirectory(directoryPath, &error);
    if (!loaded) {
        logWorkerMessage(QStringLiteral("Image directory load failed: %1").arg(error));
        emit imageDirectoryLoadFinished(false, error, -1, 0, {}, {});
        return;
    }

    logWorkerMessage(QStringLiteral("Loaded %1 images. Current=%2 size=%3x%4")
                         .arg(m_impl->imagePaths.size())
                         .arg(m_impl->currentDisplayName())
                         .arg(m_impl->currentImage.width())
                         .arg(m_impl->currentImage.height()));
    emit imageDirectoryLoadFinished(true,
                                    {},
                                    m_impl->currentImageIndex,
                                    m_impl->imagePaths.size(),
                                    m_impl->currentDisplayName(),
                                    m_impl->currentImage.size());
    emitImageSelection();
    scheduleRender(0);
}

void D3D11NativeWorker::selectNextImage()
{
    if (!m_initialized) {
        return;
    }

    QString error;
    if (!m_impl->selectRelativeImage(1, &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
        return;
    }

    emitImageSelection();
    scheduleRender(0);
}

void D3D11NativeWorker::selectPreviousImage()
{
    if (!m_initialized) {
        return;
    }

    QString error;
    if (!m_impl->selectRelativeImage(-1, &error)) {
        if (!error.isEmpty()) {
            emit initializationFailed(error);
        }
        return;
    }

    emitImageSelection();
    scheduleRender(0);
}

void D3D11NativeWorker::requestRender()
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_renderScheduled = false;
    }

    if (!m_initialized || m_slotPool == nullptr || !m_impl->hasImage()) {
        return;
    }

    int renderSlot = -1;
    if (!m_slotPool->tryAcquireRenderSlot(&renderSlot)) {
        scheduleRender(4);
        return;
    }

    const QSize size = currentOutputSize();
    QString error;
    if (!m_impl->ensureSlotResources(renderSlot, size, m_slotPool, &error)) {
        m_slotPool->abandonRenderSlot(renderSlot);
        emit initializationFailed(error);
        return;
    }

    const ImageEffectParameters parameters = [this]() {
        QMutexLocker locker(&m_stateMutex);
        return m_effectParameters;
    }();

    QElapsedTimer timer;
    timer.start();
    if (!m_impl->renderSlot(renderSlot, size, parameters, m_frameIndex, &error)) {
        m_slotPool->abandonRenderSlot(renderSlot);
        emit initializationFailed(error);
        return;
    }

    D3D11NativeFrame frame;
    if (!m_slotPool->submitRenderedFrame(renderSlot, m_frameIndex, &frame)) {
        m_slotPool->abandonRenderSlot(renderSlot);
        scheduleRender(4);
        return;
    }

    logWorkerMessage(QStringLiteral("Rendered frame=%1 slot=%2 output=%3x%4 elapsedMs=%5")
                         .arg(m_frameIndex)
                         .arg(frame.slotIndex)
                         .arg(frame.size.width())
                         .arg(frame.size.height())
                         .arg(QString::number(double(timer.nsecsElapsed()) / 1000000.0, 'f', 2)));
    emit frameReady(frame.slotIndex, frame.generation, frame.size, frame.frameIndex);
    ++m_frameIndex;
}

void D3D11NativeWorker::shutdown()
{
    m_initialized = false;
    m_renderScheduled = false;
    m_frameIndex = 0;
    m_slotPool = nullptr;
    m_impl = std::make_unique<Impl>();
}

void D3D11NativeWorker::emitImageSelection()
{
    if (!m_impl->hasImage()) {
        emit imageSelectionChanged(-1, 0, {}, {});
        return;
    }

    emit imageSelectionChanged(m_impl->currentImageIndex,
                               m_impl->imagePaths.size(),
                               m_impl->currentDisplayName(),
                               m_impl->currentImage.size());
}

QSize D3D11NativeWorker::currentOutputSize() const
{
    QMutexLocker locker(&m_stateMutex);
    return sanitizedSize(m_outputSize);
}

void D3D11NativeWorker::scheduleRender(int delayMs)
{
    QMutexLocker locker(&m_stateMutex);
    if (m_renderScheduled) {
        return;
    }

    m_renderScheduled = true;
    QTimer::singleShot(qMax(0, delayMs), this, &D3D11NativeWorker::requestRender);
}
