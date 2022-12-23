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

-- room.lua
-- 
-- This file defines room generation functions.

room = {}

-- room.gen_altar
-- Probability that an altar will generate in a room.
-- Default: 1 in 60
function room.gen_altar()
    return rng.onein(15)
end

function room.gen_fountain()
    return rng.onein(10)
end

function room.gen_sink()
    return rng.onein(60)
end

-- room.number_of_items
-- The number of items in a room.
-- ARGS:
--      size_x - The horizontal size of the room
--      size_y - The vertical size of the room
--      z      - The dungeon level (higher number => deeper)
-- RETURNS:
--      The number of items in that will be in the room.
--
-- Default: if rng.onein(3) then return rng.rne(5) else return 0 end
--          default value has a mean of 0.42 items per room
function room.number_of_items(size_x, size_y, z)
    if rng.onein(3) then return rng.rne(5) else return 0 end
end








