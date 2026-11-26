// SPDX-License-Identifier: Apache-2.0
// Frame receiver (docs/bridge-protocol.md §3.1).
//
//   sync(0xA5) version(1) type seq:u16 len:u16 payload[len] crc:u16
//
// It hunts for the sync byte, takes the next six bytes as a header and
// refuses it (back to hunting) unless version is 1 and len fits MAX_LEN.
// The payload goes into a byte RAM that the core reads afterwards. When the
// CRC has arrived, `msg_valid` pulses with crc_ok saying whether it checked;
// a frame whose CRC fails is still reported, because the protocol answers it
// (§3.1). A frame that stops for more than GAP_TICKS between two bytes is
// abandoned. While `accept` is low (the core is busy with a message) new
// frames are not started: the host waits for each reply, so none arrive.
`default_nettype none
module frame_rx #(
    parameter integer MAX_LEN = 448,       // the BL616 budget per direction
    parameter integer ADDR_BITS = 9,
    parameter integer GAP_TICKS = 27000    // 1 ms at 27 MHz
) (
    input  wire                 clk,
    input  wire                 rst,
    input  wire                 accept,
    input  wire                 byte_valid,
    input  wire [7:0]           byte_data,
    input  wire [63:0]          tick,
    output reg                  msg_valid = 1'b0,
    output reg                  msg_crc_ok = 1'b0,
    output reg  [7:0]           msg_type = 8'd0,
    output reg  [15:0]          msg_seq = 16'd0,
    output reg  [15:0]          msg_len = 16'd0,
    output reg  [63:0]          msg_t2 = 64'd0,      // tick when the sync byte arrived
    input  wire [ADDR_BITS-1:0] raddr,
    output reg  [7:0]           rdata = 8'd0
);
`include "crc16.vh"
    localparam [2:0] HUNT = 3'd0, HDR = 3'd1, PAY = 3'd2, CRCL = 3'd3, CRCH = 3'd4;
    reg [2:0]  st = HUNT;
    reg [2:0]  hidx = 3'd0;
    reg [15:0] idx = 16'd0;
    reg [15:0] crc = 16'hFFFF;
    reg [7:0]  version = 8'd0, crc_lo = 8'd0;
    reg [15:0] len = 16'd0;
    reg [31:0] gap = 32'd0;
    reg [63:0] t2 = 64'd0;
    reg [7:0]  ram [0:(1 << ADDR_BITS) - 1];

    always @(posedge clk) rdata <= ram[raddr];

    always @(posedge clk) begin
        msg_valid <= 1'b0;
        if (rst) begin
            st <= HUNT;
        end else if (st != HUNT && !byte_valid && gap >= GAP_TICKS) begin
            st <= HUNT; // the rest of the frame never came
        end else begin
            gap <= byte_valid ? 32'd0 : gap + 32'd1;
            if (byte_valid) begin
                case (st)
                HUNT:
                    if (accept && byte_data == 8'hA5) begin
                        st   <= HDR;
                        hidx <= 3'd0;
                        crc  <= 16'hFFFF;
                        t2   <= tick;
                    end
                HDR: begin
                    crc  <= crc16_byte(crc, byte_data);
                    hidx <= hidx + 3'd1;
                    case (hidx)
                    3'd0: version <= byte_data;
                    3'd1: msg_type <= byte_data;
                    3'd2: msg_seq[7:0] <= byte_data;
                    3'd3: msg_seq[15:8] <= byte_data;
                    3'd4: len[7:0] <= byte_data;
                    default: begin
                        len[15:8] <= byte_data;
                        idx <= 16'd0;
                        if (version != 8'd1 || {byte_data, len[7:0]} > MAX_LEN)
                            st <= HUNT; // not a header: the sync byte was data
                        else
                            st <= ({byte_data, len[7:0]} == 16'd0) ? CRCL : PAY;
                    end
                    endcase
                end
                PAY: begin
                    crc <= crc16_byte(crc, byte_data);
                    ram[idx[ADDR_BITS-1:0]] <= byte_data;
                    idx <= idx + 16'd1;
                    if (idx + 16'd1 == len) st <= CRCL;
                end
                CRCL: begin
                    crc_lo <= byte_data;
                    st <= CRCH;
                end
                default: begin // CRCH
                    msg_crc_ok <= ({byte_data, crc_lo} == crc);
                    msg_len    <= len;
                    msg_t2     <= t2;
                    msg_valid  <= 1'b1;
                    st <= HUNT;
                end
                endcase
            end
        end
    end
endmodule
`default_nettype wire
