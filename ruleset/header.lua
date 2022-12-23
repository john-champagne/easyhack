-- Copyright (c) 2021 John Champagne 
-- 
-- Permission to use, copy, modify, and/or distribute this software for any
-- purpose with or without fee is hereby granted.
-- 
-- THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
-- REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
-- AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
-- INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
-- LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
-- OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
-- PERFORMANCE OF THIS SOFTWARE.

-- header.lua
-- 
-- This file is run BEFORE all other files.
-- Tis file mostly defines some preliminary functions,
-- including RNG and math functions.
--
-- Don't edit this unless you know what you're doing.

--------------------
-- Math Functions --
--------------------

function math.round(x)
    return math.floor(x + 0.5)
end

----------------------
-- Random Functions --
----------------------

math.randomseed(os.time())
rng = {}

-- rng.randuniform
-- Returns a uniformly distributed random variable on
-- the interval [min,max).
-- ARGS:
--     min - The minimum value
--     max - The maximum value
function rng.randuniform(min,max)
    s = math.random() -- TODO: Change to nethack rng call
    return (s * (max - min)) + min
end

-- rng.randn
-- Returns a normally distributed random number.
-- The function uses the Box-Muller transform to compute.
-- ARGS:
--      mean - The mean of the distribution
--      dev  - The standard deviation of the distribution
function rng.randn(mean, dev)
    U1 = rng.randuniform(0,1)
    U2 = rng.randuniform(0,1)
    return mean + dev * math.sqrt(-2.0 * math.log(U1)) * math.cos(2.0 * math.pi * U2)
end

-- rng.randexp
-- Returns an exponentially distributed random variable
-- ARGS:
--      l - The rate parameter (lambda)
function rng.randexp(l)
    u = rng.randuniform(0,1)
    return -1.0 * math.log(u) / l
end

-- rng.rne
-- Returns an unbounded geometric distribution
-- The intention is to emulate the rne function in nethack,
-- except that the rne in nethack is bounded to less than 10.
-- ARGS:
--      x - The x parameter of the nethack RNE function
function rng.rne(x)
    return 1 + math.floor(rng.randexp(math.log(x)))
end


-- rng.chance
-- Returns true with a probability 'p'
-- ARGS:
--     p - The probability of returning true
--         (range is 0 to 0.999...)
function rng.chance(p)
    q = rng.randuniform(0,1)
    if p >= q then
        return true
    else
        return false
    end
end

-- rng.onein
-- Returns true 'one in N' times.
-- ARGS:
--     n - The N value
-- EXAMPLE:
--     rng.onein(4) returns true one in 4 times (25.00%)
function rng.onein(n)
    return rng.chance(1.0 / n)
end

-- rng.round
-- Rounds n such that the average of several calls
-- will be n.
-- e.g. 2.3 will round to:
--      2  (70% of the time)
--      3  (30% of the time)
--      Average = 2.3
-- ARGS:
--      n - The value to round
function rng.round(n)
    return math.floor(n + rng.randuniform(0,1))
end

