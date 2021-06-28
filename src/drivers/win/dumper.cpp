#include <Windows.h>
#include <Commctrl.h>

#include "types.h"
#include "resource.h"
#include "debugger.h"
#include "debuggersp.h"
#include "../../debug.h"
#include "asm.h"
#include "../../x6502.h"
#include "../../fceu.h"
#include "../../debug.h"
#include "../../nsf.h"
#include "../../ppu.h"
#include "../../cart.h"
#include "../../ines.h"
#include "../../asm.h"
#include "tracer.h"
#include "window.h"

extern Name* ramBankNames;
extern Name* pageNames[32];

static char* debug_str_decoration_comment;
static char* debug_decoration_comment;
static char* debug_decoration_comment_end_pos;
static char filename[257];

/**
* Dumps disassembled ROM code between startAddr and endAddr (inclusive) to a file.
* endAddr is the address of the last INSTRUCTION. This funcion will grab the operands if present,
* and may spill over a bit in the process.
* 0x80000 - 0xFFFF should get you the loaded banks. However, it's most reliable when you dump one subroutine at a time
* or already have a lot of labels.
*
* For example, say you have 2C A9 10 60, and the A9 byte is supposed to be the start of a subroutine. If the disassembler
* comes across that 2C first, it will interpret the code as:

* 2C A9 10 BIT $10A9
* 60       RTS

* Nonsense.

* But if you have a named label
* on that A9 byte, the 2C (BIT) instruction will be INTERRUPTED, and it will show up like this:

* 2C       INTERRUPTED
* my_subroutine:
* A9 10    LDA #$10
* 60       RTS
*
* There is a lot of reused logic between this and Disassemble. However, they're different enough that it would
* be more trouble than it's worth to combine them.
*/
void Dump(FILE *fout, unsigned int startAddr, unsigned int endAddr)
{
	wchar_t chr[40] = { 0 };
	wchar_t debug_wbuf[2048] = { 0 };
	int size;
	uint8 opcode[3];
	unsigned int instruction_addr;
	unsigned int addr = startAddr; // Keeps track of which address to get the operands, etc. from

	if (symbDebugEnabled)
		loadNameFiles();

	unsigned int instructions_count = 0;
	for (int addr = startAddr; addr <= endAddr;)
	{
		// PC pointer
		if (addr > 0xFFFF) break;

		instruction_addr = addr;

		if (symbDebugEnabled)
		{
			// Insert name and comment lines if present
			Name* node = findNode(getNamesPointerForAddress(addr), addr);
			if (node)
			{
				if (node->name)
				{
					// Could probably ditch these swprintf's and just do fwprintf.
					// Need to verify exactly how the various buffers are used.
					swprintf(debug_wbuf, L"%S:\n", node->name);
					fprintf(fout, "%ls", debug_wbuf);
				}
				if (node->comment)
				{
					// make a copy
					strcpy(debug_str_decoration_comment, node->comment);
					strcat(debug_str_decoration_comment, "\r\n");
					// divide the debug_str_decoration_comment into strings (Comment1, Comment2, ...)
					debug_decoration_comment = debug_str_decoration_comment;
					debug_decoration_comment_end_pos = strstr(debug_decoration_comment, "\r\n");
					while (debug_decoration_comment_end_pos)
					{
						debug_decoration_comment_end_pos[0] = 0;		// set \0 instead of \r
						debug_decoration_comment_end_pos[1] = 0;		// set \0 instead of \n
						swprintf(debug_wbuf, L"; %S\n", debug_decoration_comment);
						fprintf(fout, "%ls", debug_wbuf);

						debug_decoration_comment_end_pos += 2;
						debug_decoration_comment = debug_decoration_comment_end_pos;
						debug_decoration_comment_end_pos = strstr(debug_decoration_comment_end_pos, "\r\n");
					}
				}
			}
		}

		fprintf(fout, "%ls", L" ");

		if (addr >= 0x8000)
		{
			if (debuggerDisplayROMoffsets && GetNesFileAddress(addr) != -1)
			{
				swprintf(chr, L" %06X: ", GetNesFileAddress(addr));
			}
			else
			{
				swprintf(chr, L"%02X:%04X: ", getBank(addr), addr);
			}
		}
		else
		{
			swprintf(chr, L"  :%04X: ", addr);
		}

		// Add address
		fprintf(fout, "%ls", chr);

		size = opsize[GetMem(addr)];
		if (size == 0)
		{
			swprintf(chr, L"%02X        UNDEFINED", GetMem(addr++));
			fprintf(fout, "%ls", chr);
		}
		else
		{
			if ((addr + size) > 0xFFFF)
			{
				while (addr < 0xFFFF)
				{
					swprintf(chr, L"%02X        OVERFLOW\n", GetMem(addr++));
					fprintf(fout, "%ls", chr);
				}
				break;
			}
			Name* node;
			for (int j = 0; j < size; j++)
			{
				// Write the raw bytes of this instruction
				swprintf(chr, L"%02X ", opcode[j] = GetMem(addr++));
				fprintf(fout, "%ls", chr);
				if (j != size - 1 && (node = findNode(getNamesPointerForAddress(addr), addr)))
				{
					// We were treating this as an operand, but it's named!
					// Probably want an instruction to start here instead.
					printf("$%04X (%s) came up as an operand for instruction @ %04X\n", addr, node->name, addr - j - 1);
					size = j + 1;
					break;
				}
			}
			while (size < 3)
			{
				fprintf(fout, "%ls", L"   ");
				size++;
			}
			if (node)
			{
				// TODO: Instead of this ominous and confusing message, could print ".byte $XX $YY..."
				fprintf(fout, " INTERRUPTED");
			}
			else
			{
				static char bufferForDisassemblyWithPlentyOfStuff[64 + NL_MAX_NAME_LEN * 10]; // "plenty"
				char* _a = Disassemble(addr, opcode);
				// This isn't a trace log, so we want to remove the data after the @ or =.
				// There are lots of hardcoded sprintfs in Disassemble. This really is the easiest way.
				char* traceInfoIndex = strstr(_a, "@");
				if (traceInfoIndex)
					traceInfoIndex[-1] = 0;
				traceInfoIndex = strstr(_a, "=");
				if (traceInfoIndex)
					traceInfoIndex[-1] = 0;

				strcpy(bufferForDisassemblyWithPlentyOfStuff, _a);

				if (symbDebugEnabled)
				{ // TODO: This will add in both the default name and custom name if you have inlineAddresses enabled.
					if (symbRegNames)
						replaceRegNames(bufferForDisassemblyWithPlentyOfStuff);
					replaceNames(ramBankNames, bufferForDisassemblyWithPlentyOfStuff, NULL);
					for (int p = 0; p<ARRAY_SIZE(pageNames); p++)
						if (pageNames[p] != NULL)
							replaceNames(pageNames[p], bufferForDisassemblyWithPlentyOfStuff, NULL);
				}

				uint8 opCode = GetMem(instruction_addr);

				// special case: RTS and RTI
				if (opCode == 0x60 || opCode == 0x40)
				{
					// add "----------" to emphasize the end of subroutine
					strcat(bufferForDisassemblyWithPlentyOfStuff, " ");
					for (int j = strlen(bufferForDisassemblyWithPlentyOfStuff); j < (LOG_DISASSEMBLY_MAX_LEN - 1); ++j)
						bufferForDisassemblyWithPlentyOfStuff[j] = '-';
					bufferForDisassemblyWithPlentyOfStuff[LOG_DISASSEMBLY_MAX_LEN - 1] = 0;
				}

				// append disassembly to current line
				swprintf(debug_wbuf, L" %S", bufferForDisassemblyWithPlentyOfStuff);
				fprintf(fout, "%ls", debug_wbuf);
			}
		}
		fprintf(fout, "%ls", L"\n");
		instructions_count++;
	}
}

bool DumperInitDialog(HWND hwndDlg)
{
	debug_str_decoration_comment = (char*)malloc(NL_MAX_MULTILINE_COMMENT_LEN + 10);
	Edit_SetCueBannerText(GetDlgItem(hwndDlg, ID_DUMPER_START_ADDR), L"8000");
	Edit_SetCueBannerText(GetDlgItem(hwndDlg, ID_DUMPER_END_ADDR), L"FFFF");
	SetWindowLongPtr(GetDlgItem(hwndDlg, ID_DUMPER_START_ADDR), GWLP_WNDPROC, (LONG_PTR)FilterEditCtrlProc);
	SetWindowLongPtr(GetDlgItem(hwndDlg, ID_DUMPER_END_ADDR), GWLP_WNDPROC, (LONG_PTR)FilterEditCtrlProc);
	SendDlgItemMessage(hwndDlg, ID_DUMPER_START_ADDR, EM_SETLIMITTEXT, 6, 0);
	SendDlgItemMessage(hwndDlg, ID_DUMPER_END_ADDR, EM_SETLIMITTEXT, 6, 0);
	SendDlgItemMessage(hwndDlg, ID_DUMPER_FILEPATH, EM_SETLIMITTEXT, 256, 0);
	SetFocus(GetDlgItem(hwndDlg, ID_DUMPER_START_ADDR));
	return true;
}

bool DumperBnClicked(HWND hwndDlg, uint16 btnId, HWND hwndBtn)
{
	switch (btnId)
	{
		case ID_DUMPER_BROWSE:
			printf("Browser...\n");
			return true;
		case ID_DUMPER_GO:
			static char str[7];
			int startAddr, endAddr;

			// Nothing was entered.
			if (GetDlgItemText(hwndDlg, ID_DUMPER_START_ADDR, str, 6))
				startAddr = strtol(str, NULL, 16);
			else
				startAddr = 0x8000;

			if (GetDlgItemText(hwndDlg, ID_DUMPER_END_ADDR, str, 6))
				endAddr = strtol(str, NULL, 16);
			else
				endAddr = 0xFFFF;

			if (!GetDlgItemText(hwndDlg, ID_DUMPER_FILEPATH, filename, 256))
			{
				MessageBox(hwndDlg, "No file path entered.", "Code Dumper", MB_OK | MB_ICONINFORMATION);
				break;
			}

			FILE *fout;
			if (!(fout = fopen(filename, "w")))
			{
				MessageBox(hwndDlg, "Could not open file.", "Code Dumper", MB_OK | MB_ICONINFORMATION);
				break;
			}

			printf("Dumping $%04X - $%04X to \"%s\"...\n", startAddr, endAddr, filename);
			Dump(fout, startAddr, endAddr);
			fclose(fout);
			printf("Done.\n");
			return true;
	}
}

BOOL CALLBACK DumperCallB(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	    case WM_INITDIALOG:
			return DumperInitDialog(hwndDlg);
		case WM_CLOSE:
		case WM_QUIT:
			free(debug_str_decoration_comment);
			EndDialog(hwndDlg, 0);
			return true;
		case WM_COMMAND:
			switch (HIWORD(wParam))
			{
				case BN_CLICKED:
					return DumperBnClicked(hwndDlg, LOWORD(wParam), (HWND)lParam);
			}
			break;
	}
	return false;
}
