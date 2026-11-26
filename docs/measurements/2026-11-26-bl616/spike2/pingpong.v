// Ping-pong bridge prototype for the Tang Nano 20K (spike 2, input to the
// open-tick v0.4 protocol spec). Not open-tick code.
//
// What it does, as decided in the v0.4 design session (2026-11-26):
// - the bridge owns time: a 48-bit tick at 27 MHz and a step counter that
//   advances every STEP_CLKS ticks (1 ms on the board);
// - the host starts every exchange (ping-pong); the bridge answers once per
//   complete request, never streams;
// - type 1 requests carry setpoints (16-bit, little-endian) into a FIFO;
//   once the first type 1 request has arrived, the bridge consumes one
//   setpoint per step and counts an underrun for every step that finds the
//   FIFO empty;
// - every reply carries the tick at which the request's first byte started
//   (t_rx) and the tick at which the reply was queued (t_tx), for the
//   host's clock mapping (NTP-style).
//
// Request (host -> bridge):
//   A5, type, seq, cnt_lo, cnt_hi, rlen_lo, rlen_hi, payload[cnt], chk
//   type 1: payload = cnt/2 setpoints; type 2: payload ignored.
//   rlen: filler bytes the reply must carry (to measure reply sizes).
//   chk: sum mod 256 of every byte after A5 up to the end of the payload.
// Reply (bridge -> host), 28 + rlen + 1 bytes:
//   5A, seq, status, t_rx[6], t_tx[6], step[4], fill[2], underruns[2],
//   chk_errs, rlen_lo, rlen_hi, cur_sp[2], filler[rlen] (0, 1, 2, ...), chk
//   cur_sp: the setpoint the bridge applied at the last step (so the FIFO's
//   data path is real, not optimised away).
//   status bit 0: this request's checksum was right; bit 1: a setpoint was
//   lost because the FIFO was full (sticky); bit 2: a request arrived while
//   a reply was still being queued, and was not answered (sticky).
//   All multi-byte fields little-endian.
//
// LEDs (active low): led[0] checksum error seen, led[1] UART framing error,
// led[2] heartbeat, led[3] underrun seen, led[4] setpoint FIFO overflow,
// led[5] off.
`default_nettype none

module pingpong #(
    parameter integer CLKS_PER_BIT = 27,
    parameter integer STEP_CLKS = 27000,
    parameter integer SP_LOG2 = 10,
    parameter integer TX_LOG2 = 9
) (
    input  wire       clk,
    input  wire       uart_rx,
    output wire       uart_tx,
    output wire [5:0] led
);
    localparam integer CW = $clog2(CLKS_PER_BIT);
    localparam integer SW = $clog2(STEP_CLKS);

    reg [3:0] por = 4'd0;
    wire rst = ~por[3];
    always @(posedge clk) if (!por[3]) por <= por + 4'd1;

    // ---- Time: tick and step ----
    reg [47:0]   tick = 48'd0;
    reg [SW-1:0] step_div = {SW{1'b0}};
    reg [31:0]   step_cnt = 32'd0;
    wire step_edge = (step_div == STEP_CLKS - 1);
    always @(posedge clk) begin
        tick <= tick + 48'd1;
        step_div <= step_edge ? {SW{1'b0}} : step_div + 1'b1;
        if (step_edge) step_cnt <= step_cnt + 32'd1;
    end

    // ---- UART receiver (as in spike 1) plus a start-of-byte timestamp ----
    reg rx_s1 = 1'b1, rx_s2 = 1'b1;
    always @(posedge clk) begin
        rx_s1 <= uart_rx;
        rx_s2 <= rx_s1;
    end
    localparam [1:0] RX_IDLE = 2'd0, RX_START = 2'd1, RX_DATA = 2'd2, RX_STOP = 2'd3;
    reg [1:0]    rx_state = RX_IDLE;
    reg [CW-1:0] rx_cnt = {CW{1'b0}};
    reg [2:0]    rx_bit = 3'd0;
    reg [7:0]    rx_shift = 8'd0;
    reg          rx_valid = 1'b0;
    reg          frame_err = 1'b0;
    reg [47:0]   rx_byte_t = 48'd0; // tick at the falling edge of this byte's start bit

    always @(posedge clk) begin
        rx_valid <= 1'b0;
        if (rst) begin
            rx_state  <= RX_IDLE;
            frame_err <= 1'b0;
        end else begin
            case (rx_state)
            RX_IDLE:
                if (!rx_s2) begin
                    rx_state  <= RX_START;
                    rx_cnt    <= (CLKS_PER_BIT - 1) / 2;
                    rx_byte_t <= tick;
                end
            RX_START:
                if (rx_cnt == 0) begin
                    if (!rx_s2) begin
                        rx_state <= RX_DATA;
                        rx_cnt   <= CLKS_PER_BIT - 1;
                        rx_bit   <= 3'd0;
                    end else begin
                        rx_state <= RX_IDLE;
                    end
                end else begin
                    rx_cnt <= rx_cnt - 1'b1;
                end
            RX_DATA:
                if (rx_cnt == 0) begin
                    rx_shift <= {rx_s2, rx_shift[7:1]};
                    rx_cnt   <= CLKS_PER_BIT - 1;
                    if (rx_bit == 3'd7) rx_state <= RX_STOP;
                    else                rx_bit   <= rx_bit + 3'd1;
                end else begin
                    rx_cnt <= rx_cnt - 1'b1;
                end
            default:
                if (rx_cnt == 0) begin
                    if (rx_s2) rx_valid  <= 1'b1;
                    else       frame_err <= 1'b1;
                    rx_state <= RX_IDLE;
                end else begin
                    rx_cnt <= rx_cnt - 1'b1;
                end
            endcase
        end
    end

    // ---- Request parser ----
    localparam [3:0] P_SYNC = 4'd0, P_TYPE = 4'd1, P_SEQ = 4'd2, P_CLO = 4'd3, P_CHI = 4'd4,
                     P_RLO = 4'd5, P_RHI = 4'd6, P_PAY = 4'd7, P_CHK = 4'd8;
    reg [3:0]  p_state = P_SYNC;
    reg [7:0]  p_type = 8'd0, p_seq = 8'd0, p_sum = 8'd0;
    reg [15:0] p_cnt = 16'd0, p_left = 16'd0, p_rlen = 16'd0;
    reg [47:0] p_trx = 48'd0;
    reg [7:0]  p_lo = 8'd0;
    reg        p_odd = 1'b0;
    reg        req_done = 1'b0;   // one-cycle pulse: a complete request
    reg        req_ok = 1'b0;     // its checksum was right
    reg [7:0]  chk_errs = 8'd0;
    reg        started = 1'b0;

    // setpoint FIFO, written by the parser, read once per step
    localparam integer SP_DEPTH = 1 << SP_LOG2;
    reg [15:0] sp_mem [0:SP_DEPTH-1];
    reg [SP_LOG2:0] sp_wp = 0, sp_rp = 0;
    wire sp_empty = (sp_wp == sp_rp);
    wire sp_full  = (sp_wp[SP_LOG2] != sp_rp[SP_LOG2]) && (sp_wp[SP_LOG2-1:0] == sp_rp[SP_LOG2-1:0]);
    reg  sp_ovf = 1'b0;
    reg  sp_push = 1'b0;
    reg [15:0] sp_wd = 16'd0;

    always @(posedge clk) begin
        req_done <= 1'b0;
        sp_push  <= 1'b0;
        if (rst) begin
            p_state <= P_SYNC;
        end else if (rx_valid) begin
            if (p_state != P_SYNC && p_state != P_CHK) p_sum <= p_sum + rx_shift;
            case (p_state)
            P_SYNC: if (rx_shift == 8'hA5) begin
                        p_state <= P_TYPE;
                        p_sum   <= 8'd0;
                        p_trx   <= rx_byte_t;
                    end
            P_TYPE: begin p_type <= rx_shift; p_state <= P_SEQ; end
            P_SEQ:  begin p_seq  <= rx_shift; p_state <= P_CLO; end
            P_CLO:  begin p_cnt[7:0]  <= rx_shift; p_state <= P_CHI; end
            P_CHI:  begin p_cnt[15:8] <= rx_shift; p_state <= P_RLO; end
            P_RLO:  begin p_rlen[7:0] <= rx_shift; p_state <= P_RHI; end
            P_RHI:  begin
                        p_rlen[15:8] <= rx_shift;
                        p_left <= p_cnt;
                        p_odd  <= 1'b0;
                        p_state <= (p_cnt == 16'd0) ? P_CHK : P_PAY;
                    end
            P_PAY:  begin
                        if (p_type == 8'd1) begin
                            if (!p_odd) begin
                                p_lo <= rx_shift;
                            end else begin
                                sp_wd   <= {rx_shift, p_lo};
                                sp_push <= 1'b1;
                            end
                        end
                        p_odd  <= ~p_odd;
                        p_left <= p_left - 16'd1;
                        if (p_left == 16'd1) p_state <= P_CHK;
                    end
            default: begin // P_CHK
                        req_ok   <= (rx_shift == p_sum);
                        if (rx_shift != p_sum && chk_errs != 8'hFF) chk_errs <= chk_errs + 8'd1;
                        if (rx_shift == p_sum && p_type == 8'd1) started <= 1'b1;
                        req_done <= 1'b1;
                        p_state  <= P_SYNC;
                    end
            endcase
        end
    end

    // setpoint FIFO: write side and the per-step read side
    reg [15:0] cur_sp = 16'd0;
    reg [15:0] underruns = 16'd0;
    always @(posedge clk) begin
        if (sp_push) begin
            if (!sp_full) begin
                sp_mem[sp_wp[SP_LOG2-1:0]] <= sp_wd;
                sp_wp <= sp_wp + 1'b1;
            end else begin
                sp_ovf <= 1'b1;
            end
        end
        if (step_edge && started) begin
            if (!sp_empty) begin
                cur_sp <= sp_mem[sp_rp[SP_LOG2-1:0]];
                sp_rp  <= sp_rp + 1'b1;
            end else if (underruns != 16'hFFFF) begin
                underruns <= underruns + 16'd1;
            end
        end
    end
    wire [SP_LOG2:0] sp_fill = sp_wp - sp_rp;

    // ---- Reply sequencer -> TX FIFO ----
    localparam integer TX_DEPTH = 1 << TX_LOG2;
    reg [7:0] tx_mem [0:TX_DEPTH-1];
    reg [TX_LOG2:0] tx_wp = 0, tx_rp = 0;
    wire tx_empty = (tx_wp == tx_rp);
    wire tx_full  = (tx_wp[TX_LOG2] != tx_rp[TX_LOG2]) && (tx_wp[TX_LOG2-1:0] == tx_rp[TX_LOG2-1:0]);

    reg        r_busy = 1'b0;
    reg        r_dropped = 1'b0;
    reg [15:0] r_idx = 16'd0, r_rlen = 16'd0;
    reg [7:0]  r_seq = 8'd0, r_status = 8'd0, r_sum = 8'd0, r_cerr = 8'd0;
    reg [47:0] r_trx = 48'd0, r_ttx = 48'd0;
    reg [31:0] r_step = 32'd0;
    reg [15:0] r_fill = 16'd0, r_under = 16'd0, r_cur = 16'd0;
    localparam integer HDR = 28; // bytes before the filler

    reg [7:0] r_byte;
    always @(*) begin
        case (r_idx)
        16'd0:  r_byte = 8'h5A;
        16'd1:  r_byte = r_seq;
        16'd2:  r_byte = r_status;
        16'd3:  r_byte = r_trx[7:0];   16'd4:  r_byte = r_trx[15:8];
        16'd5:  r_byte = r_trx[23:16]; 16'd6:  r_byte = r_trx[31:24];
        16'd7:  r_byte = r_trx[39:32]; 16'd8:  r_byte = r_trx[47:40];
        16'd9:  r_byte = r_ttx[7:0];   16'd10: r_byte = r_ttx[15:8];
        16'd11: r_byte = r_ttx[23:16]; 16'd12: r_byte = r_ttx[31:24];
        16'd13: r_byte = r_ttx[39:32]; 16'd14: r_byte = r_ttx[47:40];
        16'd15: r_byte = r_step[7:0];  16'd16: r_byte = r_step[15:8];
        16'd17: r_byte = r_step[23:16]; 16'd18: r_byte = r_step[31:24];
        16'd19: r_byte = r_fill[7:0];  16'd20: r_byte = r_fill[15:8];
        16'd21: r_byte = r_under[7:0]; 16'd22: r_byte = r_under[15:8];
        16'd23: r_byte = r_cerr;
        16'd24: r_byte = r_rlen[7:0];  16'd25: r_byte = r_rlen[15:8];
        16'd26: r_byte = r_cur[7:0];   16'd27: r_byte = r_cur[15:8];
        default: r_byte = (r_idx == HDR + r_rlen) ? r_sum : (r_idx - HDR);
        endcase
    end

    always @(posedge clk) begin
        if (req_done) begin
            if (r_busy) begin
                r_dropped <= 1'b1;
            end else begin
                r_busy   <= 1'b1;
                r_idx    <= 16'd0;
                r_sum    <= 8'd0;
                r_seq    <= p_seq;
                r_status <= {5'd0, r_dropped, sp_ovf, req_ok};
                r_trx    <= p_trx;
                r_ttx    <= tick;
                r_step   <= step_cnt;
                r_fill   <= {{(15 - SP_LOG2){1'b0}}, sp_fill};
                r_under  <= underruns;
                r_cerr   <= chk_errs;
                r_rlen   <= p_rlen;
                r_cur    <= cur_sp;
            end
        end else if (r_busy && !tx_full) begin
            tx_mem[tx_wp[TX_LOG2-1:0]] <= r_byte;
            tx_wp <= tx_wp + 1'b1;
            if (r_idx != 16'd0 && r_idx != HDR + r_rlen) r_sum <= r_sum + r_byte;
            if (r_idx == HDR + r_rlen) r_busy <= 1'b0;
            r_idx <= r_idx + 16'd1;
        end
    end

    // ---- Transmitter with one-byte prefetch (as in spike 1) ----
    reg [7:0] next_byte = 8'd0;
    reg       rd_pending = 1'b0;
    reg       have_next = 1'b0;
    reg [9:0]    tx_shift = 10'h3FF;
    reg [3:0]    tx_bit = 4'd9;
    reg [CW-1:0] tx_cnt = {CW{1'b0}};
    reg          tx_busy = 1'b0;
    wire tx_frame_end = !tx_busy || (tx_bit == 4'd9 && tx_cnt == 0);
    wire load = have_next && tx_frame_end;

    always @(posedge clk) begin
        rd_pending <= 1'b0;
        if (!have_next && !rd_pending && !tx_empty) begin
            next_byte  <= tx_mem[tx_rp[TX_LOG2-1:0]];
            tx_rp      <= tx_rp + 1'b1;
            rd_pending <= 1'b1;
        end
        if (rd_pending) have_next <= 1'b1;
        if (load)       have_next <= 1'b0;
    end

    always @(posedge clk) begin
        if (load) begin
            tx_shift <= {1'b1, next_byte, 1'b0};
            tx_bit   <= 4'd0;
            tx_cnt   <= CLKS_PER_BIT - 1;
            tx_busy  <= 1'b1;
        end else if (tx_busy) begin
            if (tx_cnt == 0) begin
                tx_shift <= {1'b1, tx_shift[9:1]};
                if (tx_bit == 4'd9) begin
                    tx_busy <= 1'b0;
                end else begin
                    tx_bit <= tx_bit + 4'd1;
                    tx_cnt <= CLKS_PER_BIT - 1;
                end
            end else begin
                tx_cnt <= tx_cnt - 1'b1;
            end
        end
    end
    assign uart_tx = tx_shift[0];

    reg [24:0] beat = 25'd0;
    always @(posedge clk) beat <= beat + 25'd1;
    assign led = {1'b1, ~sp_ovf, ~(underruns != 16'd0), ~beat[24], ~frame_err, ~(chk_errs != 8'd0)};
endmodule

`default_nettype wire
