/*
 * PE.h
 *
 *  Created on: Mar 4, 2010
 *      Author: ardavan
 */

#ifndef PE_H_
#define PE_H_

#include "Parameters.h"
#include "Reg_File.h"
#include "FMA.h"
class PE
{
public:
	PE();
	PE(int row, int col, double * row_write_bus, double * row_read_bus, double * col_write_bus, double* col_read_bus, int CoreID);
	virtual ~PE();
/*	int	Rank_Update();
	int Multiply();
	int Trsm();
	int Chol();
*/
	int Execute					(int Global_index, int Trsm_index, int iter_Counter, int Latency_Counter,
								LAPU_Function routine,int LAPU_Current_State, int State_Start);
	int Generate_Address_Signals(int Global_index, int Trsm_index, int iter_Counter, int Latency_Counter,
								LAPU_Function routine,int LAPU_Current_State, int State_Start);
	// Added for Matmul

	int Execute_Matmul (int Global_index, int N, int Mc, int Kc, int Ma,  int LAPU_Current_State, int Latency_Counter, int BigA, int BigC);

  int Fetch(int IOisReady, int Comm_State, bool Stall, int Kc, int init_current);

	int Cycle();
  void Reset();
  int Gen_Address (int Global_Iteration, int LAPU_itration);

	int Intialize_Local_Mem( double ** Input_matrix, int row_number, int column_number, int offset);

	int Flush_Local_Mem( double **& Input_matrix, int row_number, int column_number, int offset);
	//TODO who keeps the offset or calculates it ? simulator (mem)

	int Initialize_Local_Mem_New( double ** Input_matrix, int row_number, int column_number, int offset, char matr);

	int Flush_Local_Mem_New( double **& Input_matrix, int row_number, int column_number, int offset, char matr);

	void Dump_PE_Mem (int amount);

	int Dump_Regs();

	int Dump_ALU_Pipeline(ALU_op operation_type);

	int Return_PE_Power();

  int Recurs_Gen_A(int Prev);
  
  int Recurs_Gen_ReadA(int Prev);

  int Recurs_Gen_B(int Prev);
  

private:


	double * Scratch_Regs_Curr;
	double * Scratch_Regs_Next;

	int My_Row, My_Column;

	// Registers Current
	//Registers Next

	int Counter_Curr;
	int Counter_Next;

	int Local_Mem_Address;

	// Address Regs Added for matmul
	int Address_Reg_A_Curr;
	int Address_Reg_A_Next;

	int Address_Reg_B_Curr;
	int Address_Reg_B_Next;

	int Address_Reg_WB_Curr;
	int Address_Reg_WB_Next;

	int Address_B;
	int Address_A;
	int Address_WB;
	int Address_WA;
	
  int Address_B_New;
	int Address_A_New;
	int Address_WB_New;
	int Address_WA_New;

	int Cin_Counter;
	int Cout_Counter;
  int Bin_Counter;
  int Ain_Counter;

	double * Read_My_Col_Bus , * Read_My_Row_Bus;
	double * Write_My_Col_Bus, * Write_My_Row_Bus;
	double Write_My_Col_Reg_Curr, Write_My_Row_Reg_Curr;
	double Write_My_Col_Reg_Next, Write_My_Row_Reg_Next;
	Reg_File Local_Mem;
	FMA ALU;

  int Address_WB_Fetch;
  char toA;
  char toB1;
  char toB2;
  bool BMode;
  bool AMode;

  bool fetch_C;
  bool fetch_A;
  bool fetch_B;
  bool send_C;

  int FcA;
  int newC;

  int MyCoreID;
  int Kernel_V;
  int Kernel_H;

};

#endif /* PE_H_ */
