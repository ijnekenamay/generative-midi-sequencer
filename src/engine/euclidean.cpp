#include "euclidean.hpp"

EuclideanGenerator::EuclideanGenerator() {
    // Constructor (no initialization needed for this helper class)
}

bool EuclideanGenerator::calculate_step(uint8_t current_step, uint8_t steps, uint8_t pulses, uint8_t shift) {
    // Guard against division by zero and invalid inputs
    if (steps == 0) return false;
    if (pulses == 0) return false;
    if (pulses >= steps) return true;
    
    // Apply rotation/shift to the step index
    // Ensures a clean, modular right-shift of the pattern
    uint8_t shifted_step = (current_step + steps - (shift % steps)) % steps;
    
    // Mathematically equivalent to Bjorklund's algorithm ( Bresenham's line algorithm )
    return (shifted_step * pulses) % steps < pulses;
}
