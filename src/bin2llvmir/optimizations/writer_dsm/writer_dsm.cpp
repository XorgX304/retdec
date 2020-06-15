/**
 * @file src/bin2llvmir/optimizations/writer_dsm/writer_dsm.cpp
 * @brief Generate the current disassembly.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 */

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

#include "retdec/utils/time.h"
#include "retdec/bin2llvmir/optimizations/writer_dsm/writer_dsm.h"

using namespace retdec::common;
using namespace retdec::utils;

#define debug_enabled false

namespace retdec {
namespace bin2llvmir {

char DsmWriter::ID = 0;

static llvm::RegisterPass<DsmWriter> X(
		"retdec-write-dsm",
		"Disassembly generation",
		 false, // Only looks at CFG
		 false // Analysis Pass
);

DsmWriter::DsmWriter() :
		ModulePass(ID)
{

}

/**
 * @return Always @c false. This pass produces DSM output, it does not modify
 *         module.
 */
bool DsmWriter::runOnModule(llvm::Module& m)
{
	_module = &m;
	_objf = FileImageProvider::getFileImage(_module);
	_config = ConfigProvider::getConfig(_module);
	if (_config == nullptr)
	{
		return false;
	}
	_abi = AbiProvider::getAbi(_module);

	// New output name.
	//
	auto out = _config->getConfig().parameters.getOutputFile();
	if (out.empty())
	{
		return false;
	}
	std::string dsmOut = out + ".dsm";
	auto lastDot = out.find_last_of('.');
	if (lastDot != std::string::npos)
	{
		dsmOut = out.substr(0, lastDot) + ".dsm";
	}

	std::ofstream outFile(dsmOut, std::ofstream::out);
	assert(outFile.is_open() && "Error in opening output dsm file");
	if (!outFile.is_open())
	{
		return false;
	}

	run(outFile);
	outFile.close();

	return false;
}

/**
 * @return Always @c false. This pass produces DSM output, it does not modify
 *         module.
 */
bool DsmWriter::runOnModuleCustom(
		llvm::Module& m,
		Config* c,
		FileImage* objf,
		Abi* abi,
		std::ostream& ret)
{
	_module = &m;
	_config = c;
	_objf = objf;
	_abi = abi;
	run(ret);
	return false;
}

void DsmWriter::run(std::ostream& ret)
{
	if (_config == nullptr || _objf == nullptr || _abi == nullptr)
	{
		return;
	}

	findLongestAddress();
	findLongestInstruction();
	generateHeader(ret);
	generateCode(ret);
	generateData(ret);
}

void DsmWriter::generateHeader(std::ostream& ret)
{
	ret << ";;\n";
	ret << ";; This file was generated by the Retargetable Decompiler\n";
	ret << ";; Website: https://retdec.com\n";
	ret << ";; Copyright (c) " << retdec::utils::getCurrentYear()
	    << " Retargetable Decompiler <info@retdec.com>\n";
	ret << ";;\n";
	ret << ";; Decompilation date: "
	    << retdec::utils::getCurrentDate() << " "
	    << retdec::utils::getCurrentTime() << "\n";
	ret << ";; Architecture: "
	    << _config->getConfig().architecture.getName() << "\n";
	ret << ";;\n";
}

void DsmWriter::generateCode(std::ostream& ret)
{
	ret << "\n";
	ret << ";;\n";
	ret << ";; Code Segment" << "\n";
	ret << ";;\n";
	ret << "\n";

	for (auto& f : _config->getConfig().functions)
	{
		if (f.getStart().isDefined())
		{
			_addr2fnc[f.getStart()] = &f;
		}
	}

	for (auto& seg : _objf->getSegments())
	{
		auto* fileformatSec = seg->getSecSeg();
		if (fileformatSec == nullptr
				|| (!fileformatSec->isCode() && !fileformatSec->isCodeAndData()))
		{
			continue;
		}

		generateCodeSeg(seg.get(), ret);
	}
}

void DsmWriter::generateCodeSeg(
		const retdec::loader::Segment* seg,
		std::ostream& ret)
{
	ret << "; section: " << seg->getName() << "\n";

	Address addr;
	for (addr = seg->getAddress(); addr < seg->getEndAddress(); )
	{
		auto fIt = _addr2fnc.find(addr);
		auto* f = fIt != _addr2fnc.end() ? fIt->second : nullptr;
		if (f)
		{
			generateFunction(f, ret);
			addr = f->getEnd() > addr ? f->getEnd() : Address(addr + 1);
			continue;
		}

		Address nextFncAddr = addr;
		while (nextFncAddr < seg->getEndAddress())
		{
			if (_addr2fnc.count(nextFncAddr))
			{
				break;
			}
			++nextFncAddr;
		}

		Address last = nextFncAddr;
		ret << "; data inside code section at "
				<< addr.toHexPrefixString() << " -- "
				<< last.toHexPrefixString() << "\n";
		generateDataRange(addr, nextFncAddr, ret);
		addr = nextFncAddr;
	}
}

void DsmWriter::generateFunction(
		const retdec::common::Function* fnc,
		std::ostream& ret)
{
	ret << ";";

	if (fnc->isStaticallyLinked())
	{
		ret << " statically linked";
	}
	else if (fnc->isDynamicallyLinked())
	{
		ret << " dynamically linked";
	}
	else if (fnc->isSyscall())
	{
		ret << " system-call";
	}
	else if (fnc->isIdiom())
	{
		assert(false && "idiom function should not have valid address");
		ret << " instruction-idiom";
	}

	ret << " function: " << getFunctionName(fnc) << " at "
			<< fnc->getStart().toHexPrefixString()
			<< " -- " << fnc->getEnd().toHexPrefixString() << "\n";

	if (!fnc->isDecompilerDefined() && !fnc->isUserDefined())
	{
		return;
	}

	auto ai = AsmInstruction(_module, fnc->getStart());
	while (ai.isValid())
	{
		generateInstruction(ai, ret);

		auto next = ai.getNext();
		if (next.isValid() && ai.getEndAddress() < next.getAddress())
		{
			ret << "; data inside code section at "
					<< ai.getEndAddress().toHexPrefixString()
					<< " -- "
					<< next.getAddress().toHexPrefixString() << "\n";
			generateDataRange(ai.getEndAddress(), next.getAddress(), ret);
		}
		else if (next.isInvalid() && ai.getEndAddress() < fnc->getEnd())
		{
			Address end = fnc->getEnd() + 1;
			ret << "; data inside code section at "
					<< ai.getEndAddress().toHexPrefixString()
					<< " -- " << end.toHexPrefixString() << "\n";
			generateDataRange(ai.getEndAddress(), end, ret);
		}

		ai = next;
	}
}

void DsmWriter::getAsmInstructionHex(AsmInstruction& ai, std::ostream& ret)
{
	std::size_t longestHexa = _longestInst * 3 - 1;
	const std::size_t aiHexa = ai.getByteSize() * 3 - 1;

	std::vector<std::uint64_t> bytes;
	if (_objf->getImage()->get1ByteArray(ai.getAddress(), bytes, ai.getByteSize()))
	{
		for (size_t i = 0; i < bytes.size(); ++i)
		{
			ret << (i == 0 ? "" : " ") << std::setw(2) << std::setfill('0')
					<< std::hex << bytes[i];
		}
	}
	else
	{
		for (size_t i = 0; i < ai.getByteSize(); ++i)
		{
			ret << (i == 0 ? "??" : " ??");
		}
	}

	const auto diff = longestHexa - aiHexa;
	for (size_t i = 0; i < diff; ++i)
	{
		ret << ' ';
	}
}

void DsmWriter::generateInstruction(AsmInstruction& ai, std::ostream& ret)
{
	generateAlignedAddress(ai.getAddress(), ret);
	getAsmInstructionHex(ai, ret);
	ret << ALIGN << INSTR_SEPARATOR << processInstructionDsm(ai) << "\n";
}

std::string DsmWriter::processInstructionDsm(AsmInstruction& ai)
{
	std::string ret = ai.getDsm();

	// Ugly and potentially dangerous hack for MIPS.
	// Because of delay slots, branches are in different (next) instructions.
	// The problem is, what if there are some calls, branches, that were not
	// created with delays slots?
	//
	AsmInstruction tmpAi = ai;
	if (_config->getConfig().architecture.isMipsOrPic32())
	{
		if (AsmInstruction nextAi = tmpAi.getNext())
		{
			tmpAi = nextAi;
		}
	}

	if (auto* c = tmpAi.getInstructionFirst<llvm::CallInst>()) // ai
	{
		if (auto* f = c->getCalledFunction())
		{
			auto addr = _config->getFunctionAddress(f);
			if (addr.isDefined())
			{
				ret += " <" + getFunctionName(f) + ">";
			}
		}
	}
	else if (auto* br = tmpAi.getInstructionFirst<llvm::BranchInst>()) // ai
	{
		bool ok = true;
		auto* falseDestUse = br->isConditional() ? br->op_end() - 2 : nullptr;
		auto* falseDestBb = falseDestUse
				? llvm::cast<llvm::BasicBlock>(falseDestUse->get())
				: nullptr;
		if (falseDestBb)
		{
			auto* falseDestI = &falseDestBb->front();
			AsmInstruction falseDestAi(falseDestI);
			if (falseDestAi == tmpAi) // ai
			{
				ok = false;
			}
		}

		auto* trueDestUse = br->op_end() - 1;
		auto* trueDestBb = llvm::cast<llvm::BasicBlock>(trueDestUse->get());
		auto* trueDestI = &trueDestBb->front();
		AsmInstruction trueDestAi(trueDestI);

		if (ok && trueDestAi.isValid() && br->isUnconditional() && trueDestAi == ai.getNext())
		{
			ok = false;
		}

		if (ok && trueDestAi.isValid() && trueDestAi != tmpAi) // ai
		{
			auto* trueDestFnc = trueDestI->getFunction();
			auto addr = _config->getFunctionAddress(trueDestFnc);
			if (trueDestAi.isValid() && addr.isDefined())
			{
				retdec::common::Address o = trueDestAi.getAddress() - addr;
				ret += " <" + getFunctionName(trueDestFnc) + "+"
						+ o.toHexPrefixString() + ">";
			}
		}
	}

	// TODO: right now, this expects only x86 DSM.
	//
	std::string comment;
	if (_config->getConfig().architecture.isX86())
	{
		auto* capstoneI = ai.getCapstoneInsn();
		auto& xi = capstoneI->detail->x86;
		for (unsigned j = 0; j < xi.op_count; ++j)
		{
			cs_x86_op& op = xi.operands[j];
			Address val;
			if (op.type == X86_OP_IMM)
			{
				val = op.imm;
			}
			else if (op.type == X86_OP_MEM
					&& op.mem.base == X86_REG_INVALID
					&& op.mem.index == X86_REG_INVALID
					&& op.mem.segment == X86_REG_INVALID
					&& op.mem.scale == 1)
			{
				val = op.mem.disp;
			}

			if (val.isDefined())
			{
				auto* cg = _config->getConfigGlobalVariable(val);
				auto* g = _config->getLlvmGlobalVariable(val);
				if (cg && g && g->hasInitializer())
				{
					if (auto* cda = llvm::dyn_cast<llvm::ConstantDataArray>(
							g->getInitializer()))
					{
						comment += " ; " + getString(cg, cda);
						break;
					}
				}
			}
		}
	}

	ret = reduceNegativeNumbers(ret);
	ret = removeConsecutiveSpaces(ret);
	ret = replaceAll(ret, " ,", ",");
	ret += comment;

	return ret;
}

void DsmWriter::generateData(std::ostream& ret)
{
	ret << "\n";
	ret << ";;\n";
	ret << ";; Data Segment" << "\n";
	ret << ";;\n";
	ret << "\n";

	for (auto& seg : _objf->getSegments())
	{
		auto* fileformatSec = seg->getSecSeg();
		if (fileformatSec == nullptr
				|| (!fileformatSec->isData() && !fileformatSec->isConstData()))
		{
			continue;
		}

		generateDataSeg(seg.get(), ret);
	}
}

void DsmWriter::generateDataSeg(
		const retdec::loader::Segment* seg,
		std::ostream& ret)
{
	ret << "; section: " << seg->getName() << "\n";
	generateDataRange(seg->getAddress(), seg->getEndAddress() + 1, ret);
}

void DsmWriter::generateDataRange(
		retdec::common::Address start,
		retdec::common::Address end,
		std::ostream& ret)
{
	auto addr = start;
	while (addr < end)
	{
		llvm::ConstantDataArray* init = nullptr;
		std::string val;

		Address gvAddr = addr;
		for (; gvAddr < end; ++gvAddr)
		{
			auto* cg = _config->getConfigGlobalVariable(gvAddr);
			auto* g = _config->getLlvmGlobalVariable(gvAddr);
			if (cg && g && g->hasInitializer())
			{
				if ((init = llvm::dyn_cast<llvm::ConstantDataArray>(
						g->getInitializer())))
				{
					val = getString(cg, init);
					break;
				}
			}
		}

		if (init)
		{
			if (addr < gvAddr)
			{
				auto sz = gvAddr - addr;
				generateData(ret, addr, sz);
				addr += sz;
			}

			auto sz = _abi->getTypeByteSize(init->getType());
			generateData(ret, addr, sz, val);
			addr += sz;
		}
		else
		{
			generateData(ret, addr, end-addr);
			addr += end - addr;
		}
	}
}

void DsmWriter::generateData(
		std::ostream& ret,
		retdec::common::Address start,
		std::size_t size,
		const std::string& objVal)
{
	Address off = 0;
	while (off < size)
	{
		std::string ascii = "|";

		generateAlignedAddress(Address(start + off), ret);

		for (std::size_t off1 = 0; off1 < DATA_SEGMENT_LINE; ++off1)
		{
			if (off+off1 < size)
			{
				std::uint64_t val = 0;
				if (_objf->getImage()->get1Byte(start + off + off1, val))
				{
					unsigned char c = val;
					ret << std::setw(2) << std::setfill('0') << std::hex << val;
					ascii += std::isprint(c) ? c : '.';
				}
				else
				{
					ret << "??";
					ascii += "?";
				}
			}
			else
			{
				ret << "  ";
				ascii += " ";
			}

			if (off1 == 7)
			{
				ret << " ";
			}

			if (off1+1 < DATA_SEGMENT_LINE)
			{
				ret << " ";
			}
		}

		ascii += "|";

		ret << ALIGN << ascii;

		if (off == 0 && !objVal.empty())
		{
			ret << ALIGN << objVal;
		}

		ret << "\n";

		off += DATA_SEGMENT_LINE;
	}
}

std::string DsmWriter::escapeString(const std::string& str)
{
	std::stringstream out;

	for (unsigned char c : str)
	{
		switch (c)
		{
			case '\a': out << "\\a"; break;
			case '\b': out << "\\b"; break;
			case '\f': out << "\\f"; break;
			case '\n': out << "\\n"; break;
			case '\r': out << "\\r"; break;
			case '\t': out << "\\t"; break;
			case '\v': out << "\\v"; break;
			case '\\': out << "\\\\"; break;
			case '\"': out << "\\\""; break;
			default:
			{
				if (std::isprint(c))
				{
					out << c;
				}
				else
				{
					out << "\\x" << std::setw(2) << std::setfill('0')
						<< std::hex << c % 256;
				}
				break;
			}
		}
	}

	return out.str();
}

/**
 * @brief Find negative numbers in additions and change it to subtractions.
 * @param str String to reduce.
 * @return Reduced string.
 */
std::string DsmWriter::reduceNegativeNumbers(const std::string& str)
{
	std::size_t pos = 0;
	std::size_t lastCharPos = str.length() - 1;
	std::string res = str;

	while((pos = res.find('+', pos)) != std::string::npos)
	{
		std::size_t plusPos = pos;

		pos++;
		while (pos <= lastCharPos && isspace(res[pos]))
		{
			pos++;
		}

		if (pos <= lastCharPos && res[pos] == '-')
		{
			std::size_t minusPos = pos;

			pos++;
			while (pos <= lastCharPos && isspace(res[pos]))
			{
				pos++;
			}

			if (pos <= lastCharPos && isdigit(res[pos]))
			{
				res.replace(plusPos, minusPos - plusPos + 1, "- ");
				pos = plusPos + 2;
			}
		}
	}

	return res;
}

void DsmWriter::generateAlignedAddress(
		retdec::common::Address addr,
		std::ostream& ret)
{
	auto as = addr.toHexPrefixString();
	if (_longestAddr > as.size())
	{
		ret << as << ":" << std::string(_longestAddr - as.size(), ' ') << ALIGN;
	}
	else
	{
		ret << as << ":" << ALIGN;
	}
}

void DsmWriter::findLongestAddress()
{
	Address max;
	for (auto& seg : _objf->getSegments())
	{
		Address e = seg->getEndAddress();
		max = max.isUndefined() ? e : std::max(max, e);
	}
	_longestAddr = max.toHexPrefixString().size();
}

void DsmWriter::findLongestInstruction()
{
	for (auto& f : _module->getFunctionList())
	{
		AsmInstruction ai;
		for (auto& b : f)
		{
			for (auto& i : b)
			{
				ai = AsmInstruction(&i);
				if (ai.isValid())
				{
					break;
				}
			}
			if (ai.isValid())
			{
				break;
			}
		}

		while (ai.isValid())
		{
			if (ai.getByteSize() > _longestInst)
			{
				_longestInst = ai.getByteSize();
			}
			ai = ai.getNext();
		}
	}
}

std::string DsmWriter::getString(
		const retdec::common::Object* cgv,
		const llvm::ConstantDataArray* cda)
{
	std::string ret;

	if (cda && cda->isCString())
	{
		ret = "\"" + escapeString(cda->getAsCString()) + "\"";
	}
	else if (cda && cgv->type.isWideString())
	{
		retdec::utils::WideStringType str;
		for (unsigned i = 0; i < cda->getNumElements(); ++i)
		{
			str.push_back(cda->getElementAsInteger(i));
		}
		if (!str.empty() && str.back() == 0)
		{
			str.pop_back();
		}
		size_t sz = _abi->getTypeBitSize(cda->getElementType());
		ret = "L\"" + asEscapedCString(str, sz) + "\"";
	}

	return ret;
}

std::string DsmWriter::getFunctionName(llvm::Function* f) const
{
	auto* cf = _config->getConfigFunction(f);
	return cf ? getFunctionName(cf) : f->getName().str();
}

std::string DsmWriter::getFunctionName(const retdec::common::Function* f) const
{
	auto& rn = f->getRealName();
	return rn.empty() ? f->getName() : rn;
}

} // namespace bin2llvmir
} // namespace retdec
