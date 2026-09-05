// RegisterFile.v - 寄存器文件
module RegisterFile #(
    parameter ADDR_WIDTH = 5,
    parameter DATA_WIDTH = 32
) (
    input  clk,
    input  [DATA_WIDTH-1:0] wdata,
    input  [ADDR_WIDTH-1:0] waddr,
    input                   wen,
    input  [ADDR_WIDTH-1:0] raddr1,
    input  [ADDR_WIDTH-1:0] raddr2,
    output [DATA_WIDTH-1:0] rdata1,
    output [DATA_WIDTH-1:0] rdata2
);
    
    reg [DATA_WIDTH-1:0] rf [2**ADDR_WIDTH-1:0];
    
    // 写操作 - x0不能被写入
    always @(posedge clk) begin
        if (wen && (waddr != 0)) begin
            rf[waddr] <= wdata;
        end
    end
    
    // 读操作 - x0始终为0
    assign rdata1 = (raddr1 == 0) ? 32'b0 : rf[raddr1];
    assign rdata2 = (raddr2 == 0) ? 32'b0 : rf[raddr2];
    
    // ============================================
    // 调试函数：打印寄存器状态
    // ============================================
    task print_regs;
        begin
            $display("  寄存器状态:");
            $display("    x0  = 0x%08X (%d)", rf[0], rf[0]);
            $display("    x1  = 0x%08X (%d)", rf[1], rf[1]);
            $display("    x2  = 0x%08X (%d)", rf[2], rf[2]);
            $display("    x3  = 0x%08X (%d)", rf[3], rf[3]);
            $display("    x4  = 0x%08X (%d)", rf[4], rf[4]);
            $display("    x5  = 0x%08X (%d)", rf[5], rf[5]);
            $display("    x10 = 0x%08X (%d)", rf[10], rf[10]);
            $display("    x11 = 0x%08X (%d)", rf[11], rf[11]);
            $display("    x12 = 0x%08X (%d)", rf[12], rf[12]);
            $display("    x13 = 0x%08X (%d)", rf[13], rf[13]);
            $display("    x14 = 0x%08X (%d)", rf[14], rf[14]);
        end
    endtask
    
    task print_final_regs;
        begin
            $display("  x0  = 0x%08X (%d)", rf[0], rf[0]);
            $display("  x1  = 0x%08X (%d)", rf[1], rf[1]);
            $display("  x2  = 0x%08X (%d)", rf[2], rf[2]);
            $display("  x3  = 0x%08X (%d)", rf[3], rf[3]);
            $display("  x4  = 0x%08X (%d)", rf[4], rf[4]);
            $display("  x5  = 0x%08X (%d)", rf[5], rf[5]);
            $display("  x10 = 0x%08X (%d)", rf[10], rf[10]);
            $display("  x11 = 0x%08X (%d)", rf[11], rf[11]);
            $display("  x12 = 0x%08X (%d)", rf[12], rf[12]);
            $display("  x13 = 0x%08X (%d)", rf[13], rf[13]);
            $display("  x14 = 0x%08X (%d)", rf[14], rf[14]);
        end
    endtask
    
endmodule
