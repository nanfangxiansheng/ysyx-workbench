// LSU.v - Load-Store Unit
// 存储器本体在C++侧且按字节编址: mem_data就是从addr开始的
// 连续字节流(小端序), 因此load只需提取/扩展低字节, 无需旋转对齐
module LSU (
    input  [31:0] addr,          // 访存地址 (仅保留用于注释说明, 不再参与对齐)
    input  [2:0]  funct3,
    input  [31:0] mem_data,      // 从存储器(C++侧)读出的从addr开始的4字节
    output [31:0] load_data      // 加载到寄存器的数据
);

    reg [31:0] load_data_reg;
    assign load_data = load_data_reg;

    // 加载数据处理
    always @(*) begin
        case (funct3)
            3'b010: begin  // lw - 加载32位
                load_data_reg = mem_data;
            end
            3'b000: begin  // lb - 加载1字节, 符号扩展
                load_data_reg = {{24{mem_data[7]}}, mem_data[7:0]};
            end
            3'b100: begin  // lbu - 加载1字节, 零扩展
                load_data_reg = {24'b0, mem_data[7:0]};
            end
            3'b001: begin  // lh - 加载2字节, 符号扩展
                load_data_reg = {{16{mem_data[15]}}, mem_data[15:0]};
            end
            3'b101: begin  // lhu - 加载2字节, 零扩展
                load_data_reg = {16'b0, mem_data[15:0]};
            end
            default: begin
                load_data_reg = 32'b0;
            end
        endcase
    end

endmodule
