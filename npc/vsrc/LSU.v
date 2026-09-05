// LSU.v - Load-Store Unit
// 存储器本体在C++侧, LSU只负责对读回的对齐数据做字节选择
module LSU (
    input  [31:0] addr,          // 访存地址(用于字节选择)
    input  [2:0]  funct3,
    input  [31:0] mem_data,      // 从存储器(C++侧)读出的对齐数据
    output [31:0] load_data      // 加载到寄存器的数据
);

    reg [31:0] load_data_reg;
    assign load_data = load_data_reg;

    // 加载数据处理
    always @(*) begin
        case (funct3) // 对于加载指令,
            3'b010: begin  // lw - 加载32位
                load_data_reg = mem_data;
            end
            3'b100: begin  // lbu - 加载8位无符号
                case (addr[1:0])
                    2'b00: load_data_reg = {24'b0, mem_data[7:0]};
                    2'b01: load_data_reg = {24'b0, mem_data[15:8]};
                    2'b10: load_data_reg = {24'b0, mem_data[23:16]};
                    2'b11: load_data_reg = {24'b0, mem_data[31:24]};
                endcase
            end
            default: begin
                load_data_reg = 32'b0;
            end
        endcase
    end

endmodule
