#pragma once
#include <stdint.h>

class EuclideanGenerator {
public:
    EuclideanGenerator();
    
    /**
     * Calculates whether a trigger occurs at the current step for a given Euclidean pattern.
     * Uses a lightweight implementation of the Bjorklund algorithm.
     * 
     * @param current_step The current active step in the sequence (0-indexed).
     * @param steps Total length of the sequence (e.g. 16 steps).
     * @param pulses The density (number of active triggers/pulses).
     * @param shift Offset/rotation of the sequence.
     * @return true if there is a trigger at this step, false otherwise.
     */
    bool calculate_step(uint8_t current_step, uint8_t steps, uint8_t pulses, uint8_t shift);
};
