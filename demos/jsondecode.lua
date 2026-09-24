-- jsondecode.lua
--
-- Hyprland's Lua sandbox has no json.decode (confirmed) - this is a
-- plain recursive-descent parser, just enough for hyprctl's/the
-- notification daemon's own output (objects/arrays/strings/numbers/
-- booleans/null). Factored out of demos/which-key.lua once a second
-- demo (demos/notification-manager) needed the same thing - see that
-- file's own former copy of this comment.
local M = {}

function M.decode(str)
	local pos = 1
	local parseValue

	local function skipWhitespace()
		while pos <= #str do
			local c = str:sub(pos, pos)
			if c == " " or c == "\t" or c == "\n" or c == "\r" then
				pos = pos + 1
			else
				break
			end
		end
	end

	local function parseString()
		pos = pos + 1 -- opening quote
		local start = pos
		local buf = {}
		while pos <= #str do
			local c = str:sub(pos, pos)
			if c == '"' then
				table.insert(buf, str:sub(start, pos - 1))
				pos = pos + 1
				return table.concat(buf)
			elseif c == "\\" then
				table.insert(buf, str:sub(start, pos - 1))
				local nextC = str:sub(pos + 1, pos + 1)
				local escapes =
					{ ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" }
				if escapes[nextC] then
					table.insert(buf, escapes[nextC])
					pos = pos + 2
				elseif nextC == "u" then
					-- Minimal \uXXXX handling - only the plain-ASCII range
					-- decodes to a real character, anything above falls
					-- back to "?" rather than implementing UTF-8 encoding
					-- for fields that are ASCII in practice.
					local hex = str:sub(pos + 2, pos + 5)
					local codepoint = tonumber(hex, 16) or 63
					table.insert(buf, codepoint < 128 and string.char(codepoint) or "?")
					pos = pos + 6
				else
					table.insert(buf, nextC)
					pos = pos + 2
				end
				start = pos
			else
				pos = pos + 1
			end
		end
		error("jsondecode: unterminated string")
	end

	local function parseNumber()
		local start = pos
		while pos <= #str and str:sub(pos, pos):match("[%d%.%-%+eE]") do
			pos = pos + 1
		end
		return tonumber(str:sub(start, pos - 1))
	end

	local function parseArray()
		pos = pos + 1
		skipWhitespace()
		local arr = {}
		if str:sub(pos, pos) == "]" then
			pos = pos + 1
			return arr
		end
		while true do
			skipWhitespace()
			table.insert(arr, parseValue())
			skipWhitespace()
			local c = str:sub(pos, pos)
			if c == "," then
				pos = pos + 1
			elseif c == "]" then
				pos = pos + 1
				break
			else
				error("jsondecode: expected , or ] in array")
			end
		end
		return arr
	end

	local function parseObject()
		pos = pos + 1
		skipWhitespace()
		local obj = {}
		if str:sub(pos, pos) == "}" then
			pos = pos + 1
			return obj
		end
		while true do
			skipWhitespace()
			if str:sub(pos, pos) ~= '"' then
				error("jsondecode: expected string key in object")
			end
			local key = parseString()
			skipWhitespace()
			if str:sub(pos, pos) ~= ":" then
				error("jsondecode: expected ':' in object")
			end
			pos = pos + 1
			skipWhitespace()
			obj[key] = parseValue()
			skipWhitespace()
			local c = str:sub(pos, pos)
			if c == "," then
				pos = pos + 1
			elseif c == "}" then
				pos = pos + 1
				break
			else
				error("jsondecode: expected , or } in object")
			end
		end
		return obj
	end

	parseValue = function()
		skipWhitespace()
		local c = str:sub(pos, pos)
		if c == '"' then
			return parseString()
		elseif c == "{" then
			return parseObject()
		elseif c == "[" then
			return parseArray()
		elseif c == "t" and str:sub(pos, pos + 3) == "true" then
			pos = pos + 4
			return true
		elseif c == "f" and str:sub(pos, pos + 4) == "false" then
			pos = pos + 5
			return false
		elseif c == "n" and str:sub(pos, pos + 3) == "null" then
			pos = pos + 4
			return nil
		else
			return parseNumber()
		end
	end

	skipWhitespace()
	return parseValue()
end

return M
