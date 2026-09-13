-- BH61 recovered-radio dissector for PCAP DLT_USER0 exports.
-- Install in the Wireshark personal plugins directory, then reload Lua plugins.

local bh61 = Proto("bh61", "Verkada BH61 Recovered Radio")
local f_phr = ProtoField.uint8("bh61.phr.length", "PHR length", base.DEC)
local f_fcf = ProtoField.uint16("bh61.vmac.frame_control", "VMAC frame control", base.HEX)
local f_seq = ProtoField.uint8("bh61.vmac.sequence", "VMAC sequence", base.DEC)
local f_pan = ProtoField.uint16("bh61.vmac.destination_pan", "Destination PAN", base.HEX)
local f_vcmp = ProtoField.uint8("bh61.vcmp.type", "VCMP type", base.DEC)
local f_fcs = ProtoField.uint16("bh61.radio_fcs", "Radio FCS", base.HEX)
local f_fcs_ok = ProtoField.bool("bh61.radio_fcs_valid", "Radio FCS valid")
local f_payload = ProtoField.bytes("bh61.payload", "Payload")
bh61.fields = {f_phr, f_fcf, f_seq, f_pan, f_vcmp, f_fcs, f_fcs_ok, f_payload}

local function reverse8(value)
  local result = 0
  for _ = 1, 8 do
    result = result * 2 + (value % 2)
    value = math.floor(value / 2)
  end
  return result
end

local function radio_crc(buffer, first, count)
  local remainder = 0
  for offset = first, first + count - 1 do
    remainder = bit.bxor(remainder, bit.lshift(reverse8(buffer(offset, 1):uint()), 8))
    for _ = 1, 8 do
      if bit.band(remainder, 0x8000) ~= 0 then
        remainder = bit.bxor(bit.lshift(remainder, 1), 0x1021)
      else
        remainder = bit.lshift(remainder, 1)
      end
      remainder = bit.band(remainder, 0xffff)
    end
  end
  return remainder
end

function bh61.dissector(buffer, pinfo, tree)
  if buffer:len() < 1 then return 0 end
  local phr = buffer(0, 1):uint()
  local expected = phr + 1
  pinfo.cols.protocol = "BH61"
  local root = tree:add(bh61, buffer(), "BH61 recovered radio frame")
  root:add(f_phr, buffer(0, 1))
  if phr < 5 or phr > 127 or buffer:len() ~= expected then
    root:add_expert_info(PI_MALFORMED, PI_ERROR,
      "Buffer length must equal PHR + 1 and PHR must be 5 through 127")
    return buffer:len()
  end

  local psdu_length = phr - 2
  local fcs_offset = 1 + psdu_length
  local received_fcs = buffer(fcs_offset, 2):uint()
  local computed_fcs = radio_crc(buffer, 1, psdu_length)
  root:add(f_fcs, buffer(fcs_offset, 2))
  root:add(f_fcs_ok, received_fcs == computed_fcs)

  if psdu_length >= 5 then
    root:add_le(f_fcf, buffer(1, 2))
    root:add(f_seq, buffer(3, 1))
    root:add_le(f_pan, buffer(4, 2))
    local fcf = buffer(1, 2):le_uint()
    local destination_mode = bit.band(fcf, 0x0c00)
    local header_length = destination_mode == 0x0800 and 15 or
                          (destination_mode == 0x0c00 and 21 or 0)
    if header_length > 0 and psdu_length > header_length then
      local payload_length = psdu_length - header_length
      root:add(f_payload, buffer(1 + header_length, payload_length))
      root:add(f_vcmp, buffer(1 + header_length, 1))
    end
  end
  return buffer:len()
end

-- DLT_USER0 is exposed as wtap.USER0 on current Wireshark builds. The fallback
-- name documents compatibility with builds that expose wtap_encap.USER0.
local user0 = (wtap and wtap.USER0) or (wtap_encap and wtap_encap.USER0) or 147
DissectorTable.get("wtap_encap"):add(user0, bh61)
