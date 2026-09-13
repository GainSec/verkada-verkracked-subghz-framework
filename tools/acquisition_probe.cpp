#include "bh61/dsp/acquisition.hpp"

#include <complex>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: acquisition_probe <cf32> [carrier_hz ...]\n";
    return 2;
  }

  std::ifstream input(argv[1], std::ios::binary);
  if (!input) {
    std::cerr << "cannot open input\n";
    return 2;
  }
  input.seekg(0, std::ios::end);
  const auto bytes = input.tellg();
  input.seekg(0, std::ios::beg);
  std::vector<std::complex<float>> samples(
      static_cast<std::size_t>(bytes) / sizeof(std::complex<float>));
  input.read(reinterpret_cast<char*>(samples.data()), bytes);

  bh61::dsp::AcquisitionOptions options;
  options.minimum_score = 0.0;
  options.carrier_hypotheses_hz.clear();
  if (argc == 2) {
    options.carrier_hypotheses_hz.push_back(0.0);
  } else {
    for (int index = 2; index < argc; ++index) {
      options.carrier_hypotheses_hz.push_back(std::stod(argv[index]));
    }
  }
  const auto result =
      bh61::dsp::acquire_training(samples, 4'000'000U, options);
  if (std::holds_alternative<bh61::dsp::AcquisitionError>(result)) {
    std::cerr << std::get<bh61::dsp::AcquisitionError>(result).reason << '\n';
    return 1;
  }
  const auto& hit = std::get<bh61::dsp::AcquisitionHypothesis>(result);
  std::cout << "start_sample=" << hit.start_sample
            << " conjugated=" << hit.conjugated
            << " carrier_correction_hz=" << hit.carrier_offset_hz
            << " phase_radians=" << hit.phase_radians
            << " score=" << hit.score
            << " runner_up_score=" << hit.runner_up_score << '\n';
}
