// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "AvifDecompressor.h"

#include <spectrum/core/Constants.h>
#include <spectrum/core/SpectrumEnforce.h>
#include <spectrum/image/Scanline.h>
#include <spectrum/io/IImageSource.h>
#include <spectrum/plugins/avif/LibAvifTranscodingPlugin.h>

#include <avif/avif.h>
#include <folly/ScopeGuard.h>

namespace facebook {
namespace spectrum {
namespace plugins {
namespace avif {

namespace {

std::vector<std::uint8_t> readEntireImageSource(io::IImageSource& source) {
  std::vector<std::uint8_t> out{};
  out.reserve(source.available());

  std::vector<std::uint8_t> buffer(core::DefaultBufferSize);
  std::size_t bytesRead;
  while ((bytesRead = source.read(
              reinterpret_cast<char* const>(buffer.data()), buffer.size())) >
         0) {
    out.insert(out.end(), buffer.begin(), buffer.begin() + bytesRead);
  }

  return out;
}

} // namespace

AvifDecompressor::AvifDecompressor(io::IImageSource& source)
    : _source(source) {}

//
// Private
//

void AvifDecompressor::_ensureEntireImageIsRead() {
  if (_entireImageHasBeenRead) {
    return;
  }
  _entireImageHasBeenRead = true;

  auto image{avifImageCreateEmpty()};
  SCOPE_EXIT {
    avifImageDestroy(image);
  };

  {
    auto decoder{avifDecoderCreate()};
    SCOPE_EXIT {
      avifDecoderDestroy(decoder);
    };

    const auto payload{readEntireImageSource(_source)};
    avifROData input{payload.data(), payload.size()};
    SPECTRUM_ERROR_CSTR_IF_NOT(
        AVIF_RESULT_OK == avifDecoderRead(decoder, image, &input),
        codecs::error::DecompressorFailure,
        "failed avifDecoderRead");
  }

  SPECTRUM_ERROR_CSTR_IF_NOT(
      image->depth == 8,
      codecs::error::DecompressorFailure,
      "can only read 8-bit images");

  avifImageYUVToRGB(image);

  _entireImage.reserve(image->height);
  for (auto row = 0; row < image->height; ++row) {
    auto scanline = std::make_unique<image::Scanline>(
        image::pixel::specifications::RGB, image->width);

    const auto rgbImageOffset = row * image->width;
    auto writePtr = scanline->data();
    for (auto i = 0; i < image->width; ++i, writePtr += 3) {
      writePtr[0] = image->rgbPlanes[0][rgbImageOffset + i];
      writePtr[1] = image->rgbPlanes[1][rgbImageOffset + i];
      writePtr[2] = image->rgbPlanes[2][rgbImageOffset + i];
    }

    _entireImage.push_back(std::move(scanline));
  }

  _imageSpecification = image::Specification{
      .size = image::Size{image->width, image->height},
      .format = image::formats::Avif,
      .pixelSpecification = image::pixel::specifications::RGB};
}

//
// Public
//

image::Specification AvifDecompressor::sourceImageSpecification() {
  _ensureEntireImageIsRead();
  return *_imageSpecification;
}

image::Specification AvifDecompressor::outputImageSpecification() {
  return sourceImageSpecification();
}

std::unique_ptr<image::Scanline> AvifDecompressor::readScanline() {
  _ensureEntireImageIsRead();
  std::unique_ptr<image::Scanline> result;

  if (_currentOutputScanline < _imageSpecification->size.height) {
    result = std::move(_entireImage[_currentOutputScanline]);
    _currentOutputScanline++;
  }

  // free memory early
  if (_currentOutputScanline == _imageSpecification->size.height) {
    _entireImage.clear();
  }

  return result;
}

} // namespace avif
} // namespace plugins
} // namespace spectrum
} // namespace facebook
