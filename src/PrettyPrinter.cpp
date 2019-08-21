//===- PrettyPrinter.cpp ----------------------------------------*- C++ -*-===//
//
//  Copyright (C) 2018 GrammaTech, Inc.
//
//  This code is licensed under the MIT license. See the LICENSE file in the
//  project root for license terms.
//
//  This project is sponsored by the Office of Naval Research, One Liberty
//  Center, 875 N. Randolph Street, Arlington, VA 22203 under contract #
//  N68335-17-C-0700.  The content of the information does not necessarily
//  reflect the position or policy of the Government and no official
//  endorsement should be inferred.
//
//===----------------------------------------------------------------------===//
#include "PrettyPrinter.h"

#include "string_utils.h"
#include <boost/algorithm/string/replace.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <capstone/capstone.h>
#include <fstream>
#include <gtirb/gtirb.hpp>
#include <iomanip>
#include <iostream>
#include <utility>
#include <variant>

using namespace std::rel_ops;

template <class T> T* nodeFromUUID(gtirb::Context& C, gtirb::UUID id) {
  return dyn_cast_or_null<T>(gtirb::Node::getByUUID(C, id));
}

static std::map<std::tuple<std::string, std::string>, ::gtirb_pprint::factory>&
getFactories() {
  static std::map<std::tuple<std::string, std::string>, gtirb_pprint::factory>
      factories;
  return factories;
}

namespace gtirb_pprint {

bool registerPrinter(std::initializer_list<std::string> formats,
                     std::initializer_list<std::string> syntaxes, factory f) {
  assert(f && "Cannot register null factory!");
  assert(formats.size() > 0 && "No formats to register!");
  assert(syntaxes.size() > 0 && "No syntaxes to register!");
  for (const std::string& format : formats)
    for (const std::string& syntax : syntaxes)
      getFactories()[std::make_tuple(format, syntax)] = f;
  return true;
}

std::set<std::tuple<std::string, std::string>> getRegisteredTargets() {
  std::set<std::tuple<std::string, std::string>> targets;
  for (const auto& entry : getFactories())
    targets.insert(entry.first);
  return targets;
}

std::string getIRFileFormat(const gtirb::IR& ir) {
  switch ((*ir.modules().begin()).getFileFormat()) {
  case gtirb::FileFormat::Undefined:
    return "undefined";
  case gtirb::FileFormat::COFF:
    return "coff";
  case gtirb::FileFormat::ELF:
    return "elf";
  case gtirb::FileFormat::PE:
    return "pe";
  case gtirb::FileFormat::IdaProDb32:
  case gtirb::FileFormat::IdaProDb64:
    return "idb";
  case gtirb::FileFormat::XCOFF:
    return "xcoff";
  case gtirb::FileFormat::MACHO:
    return "macho";
  case gtirb::FileFormat::RAW:
    return "raw";
  }
  return "undefined";
}

std::string getDefaultSyntax(const std::string& format) {
  static const std::map<std::string, std::string> defaults = {
      {"elf", "intel"},
      {"pe", "masm"},
  };
  auto it = defaults.find(format);
  return it != defaults.end() ? it->second : "???";
}

PrettyPrinter::PrettyPrinter()
    : m_keep_funcs{}, m_format{"elf"}, m_syntax{"intel"}, m_debug{NoDebug} {}

void PrettyPrinter::setTarget(
    const std::tuple<std::string, std::string>& target) {
  assert(getFactories().find(target) != getFactories().end());
  const auto [format, syntax] = target;
  m_format = format;
  m_syntax = syntax;
}

void PrettyPrinter::setDebug(bool do_debug) {
  m_debug = do_debug ? DebugMessages : NoDebug;
}

bool PrettyPrinter::getDebug() const { return m_debug == DebugMessages; }

void PrettyPrinter::keepFunction(const std::string& functionName) {
  m_keep_funcs.insert(functionName);
}

std::error_condition PrettyPrinter::print(std::ostream& stream,
                                          gtirb::Context& context,
                                          gtirb::IR& ir) const {
  const auto target = std::make_tuple(m_format, m_syntax);
  getFactories().at(target)(context, ir, m_keep_funcs, m_debug)->print(stream);
  return std::error_condition{};
}

PrettyPrinterBase::PrettyPrinterBase(gtirb::Context& context_, gtirb::IR& ir_,
                                     DebugStyle dbg)
    : debug(dbg == DebugMessages ? true : false), context(context_), ir(ir_),
      functionEntry() {
  [[maybe_unused]] cs_err err =
      cs_open(CS_ARCH_X86, CS_MODE_64, &this->csHandle);
  assert(err == CS_ERR_OK && "Capstone failure");

  if (const auto* functionEntries =
          ir.modules()
              .begin()
              ->getAuxData<std::map<gtirb::UUID, std::set<gtirb::UUID>>>(
                  "functionEntries")) {
    for (auto const& function : *functionEntries) {
      for (auto& entryBlockUUID : function.second) {
        auto block = nodeFromUUID<gtirb::Block>(context, entryBlockUUID);
        functionEntry.push_back(block->getAddress());
      }
    }
    std::sort(functionEntry.begin(), functionEntry.end());
  }
}

PrettyPrinterBase::~PrettyPrinterBase() { cs_close(&this->csHandle); }

const gtirb::SymAddrConst* PrettyPrinterBase::getSymbolicImmediate(
    const gtirb::SymbolicExpression* symex) {
  if (symex) {
    const auto* s = std::get_if<gtirb::SymAddrConst>(symex);
    assert(s != nullptr && "symbolic operands must be 'address[+offset]'");
    return s;
  }
  return nullptr;
}

std::ostream& PrettyPrinterBase::print(std::ostream& os) {
  printHeader(os);
  // FIXME: simplify once block interation order is guaranteed by gtirb
  const gtirb::Module& module = *this->ir.modules().begin();
  auto address_order_block = [](const gtirb::Block* a, const gtirb::Block* b) {
    return a->getAddress() < b->getAddress();
  };
  std::vector<const gtirb::Block*> blocks;
  for (const gtirb::Block& block : gtirb::blocks(module.getCFG())) {
    blocks.push_back(&block);
  }
  std::sort(blocks.begin(), blocks.end(), address_order_block);
  auto blockIt = blocks.begin();
  auto dataIt = module.data_begin();
  gtirb::Addr last{0};
  while (blockIt != blocks.end() && dataIt != module.data_end()) {
    if ((*blockIt)->getAddress() <= dataIt->getAddress()) {
      last = printBlockOrWarning(os, **blockIt, last);
      blockIt++;
    } else {
      last = printDataObjectOrWarning(os, *dataIt, last);
      dataIt++;
    }
  }
  for (; blockIt != blocks.end(); blockIt++)
    last = printBlockOrWarning(os, **blockIt, last);
  for (; dataIt != module.data_end(); dataIt++)
    last = printDataObjectOrWarning(os, *dataIt, last);
  printSymbolDefinitionsAtAddress(os, last);
  printSectionFooter(os, std::nullopt, last);
  printFooter(os);
  return os;
}

gtirb::Addr PrettyPrinterBase::printBlockOrWarning(std::ostream& os,
                                                   const gtirb::Block& block,
                                                   gtirb::Addr last) {
  gtirb::Addr nextAddr = block.getAddress();
  if (nextAddr < last) {
    printOverlapWarning(os, nextAddr);
    return last;
  } else {
    if (nextAddr > last) {
      printSymbolDefinitionsAtAddress(os, last);
    }
    printSectionFooter(os, nextAddr, last);
    printSectionHeader(os, nextAddr);
    printBlock(os, block);
    return block.getAddress() + block.getSize();
  }
}

gtirb::Addr PrettyPrinterBase::printDataObjectOrWarning(
    std::ostream& os, const gtirb::DataObject& dataObject, gtirb::Addr last) {
  gtirb::Addr nextAddr = dataObject.getAddress();
  if (nextAddr < last) {
    printOverlapWarning(os, nextAddr);
    return last;
  } else {
    if (nextAddr > last)
      printSymbolDefinitionsAtAddress(os, last);
    printSectionFooter(os, nextAddr, last);
    printSectionHeader(os, nextAddr);
    printDataObject(os, dataObject);
    return dataObject.getAddress() + dataObject.getSize();
  }
}

void PrettyPrinterBase::printOverlapWarning(std::ostream& os,
                                            const gtirb::Addr addr) {
  os << syntax[Asm::Style::Comment]
     << " WARNING: found overlapping element at address " << std::hex
     << static_cast<uint64_t>(addr) << ": " << std::dec;
}

void PrettyPrinterBase::printBlock(std::ostream& os, const gtirb::Block& x) {
  if (skipEA(x.getAddress())) {
    return;
  }
  printFunctionHeader(os, x.getAddress());
  os << '\n';

  cs_insn* insn;
  cs_option(this->csHandle, CS_OPT_DETAIL, CS_OPT_ON);

  gtirb::ImageByteMap::const_range bytes2 =
      getBytes(this->ir.modules().begin()->getImageByteMap(), x);
  size_t count =
      cs_disasm(this->csHandle, reinterpret_cast<const uint8_t*>(&bytes2[0]),
                bytes2.size(), static_cast<uint64_t>(x.getAddress()), 0, &insn);

  // Exception-safe cleanup of instructions
  std::unique_ptr<cs_insn, std::function<void(cs_insn*)>> freeInsn(
      insn, [count](cs_insn* i) { cs_free(i, count); });

  gtirb::Offset offset(x.getUUID(), 0);
  for (size_t i = 0; i < count; i++) {
    printInstruction(os, insn[i], offset);
    offset.Displacement += insn[i].size;
    os << '\n';
  }
  // print any CFI directives located at the end of the block
  // e.g. '.cfi_endproc' is usually attached to the end of the block
  printCFIDirectives(os, offset);
}

void PrettyPrinterBase::printSectionHeader(std::ostream& os,
                                           const gtirb::Addr addr) {
  const auto found_section = this->ir.modules().begin()->findSection(addr);
  if (found_section.begin() == found_section.end())
    return;
  if (found_section.begin()->getAddress() != addr)
    return;
  std::string sectionName = found_section.begin()->getName();
  if (skip_sects.count(sectionName))
    return;
  os << '\n';
  printBar(os);
  if (sectionName == syntax[Asm::Section::Text]) {
    os << syntax[Asm::Directive::Text] << '\n';
  } else if (sectionName == syntax[Asm::Section::Data]) {
    os << syntax[Asm::Directive::Data] << '\n';
  } else if (sectionName == syntax[Asm::Section::BSS]) {
    os << syntax[Asm::Directive::BSS] << '\n';
  } else {
    printSectionHeaderDirective(os, *(found_section.begin()));
    printSectionProperties(os, *(found_section.begin()));
    os << std::endl;
  }
  if (skip_data.count(sectionName))
    os << syntax[Asm::Directive::Align] << " 8\n";
  else
    printAlignment(os, addr);
  printBar(os);
  os << '\n';
}

void PrettyPrinterBase::printSectionFooter(
    std::ostream& os, const std::optional<const gtirb::Addr> addr,
    const gtirb::Addr last) {

  const auto prev_section = getContainerSection(last - 1);
  if (!prev_section.has_value())
    return;

  std::string section_name = (*prev_section)->getName();
  if (skip_sects.count(section_name))
    return;

  const auto next_section = addr ? getContainerSection(*addr) : std::nullopt;
  if (!next_section.has_value() || !(next_section == prev_section)) {
    if (section_name == syntax[Asm::Section::Text]) {
      return;
    } else if (section_name == syntax[Asm::Section::Data]) {
      return;
    } else if (section_name == syntax[Asm::Section::BSS]) {
      return;
    } else {
      printBar(os);
      printSectionFooterDirective(os, **prev_section);
      os << std::endl;
      printBar(os);
    }
  }
}

void PrettyPrinterBase::printBar(std::ostream& os, bool heavy) {
  if (heavy) {
    os << syntax[Asm::Style::Comment]
       << "===================================\n";
  } else {
    os << syntax[Asm::Style::Comment]
       << "===================================\n";
  }
}

void PrettyPrinterBase::printSymbolReference(std::ostream& os,
                                             const gtirb::Symbol* symbol,
                                             bool isAbsolute) const {
  std::optional<std::string> forwardedName =
      getForwardedSymbolName(symbol, isAbsolute);
  if (forwardedName) {
    os << forwardedName.value();
    return;
  }
  if (symbol->getAddress() && skipEA(*symbol->getAddress())) {
    os << static_cast<uint64_t>(*symbol->getAddress());
    return;
  }
  if (this->isAmbiguousSymbol(symbol->getName()))
    os << GetSymbolToPrint(*symbol->getAddress());
  else
    os << AvoidRegNameConflicts(symbol->getName());
}

void PrettyPrinterBase::printSymbolDefinitionsAtAddress(std::ostream& os,
                                                        gtirb::Addr ea) {
  for (const gtirb::Symbol& symbol :
       this->ir.modules().begin()->findSymbols(ea)) {
    if (this->isAmbiguousSymbol(symbol.getName()))
      os << GetSymbolToPrint(*symbol.getAddress()) << ":\n";
    else
      os << AvoidRegNameConflicts(symbol.getName()) << ":\n";
  }
}

void PrettyPrinterBase::printInstruction(std::ostream& os, const cs_insn& inst,
                                         const gtirb::Offset& offset) {

  gtirb::Addr ea(inst.address);
  printSymbolDefinitionsAtAddress(os, ea);
  printComments(os, offset, inst.size);
  printCFIDirectives(os, offset);
  printEA(os, ea);

  ////////////////////////////////////////////////////////////////////
  // special cases

  if (inst.id == X86_INS_NOP) {
    os << "  " << syntax[Asm::Directive::NOP];
    for (uint64_t i = 1; i < inst.size; ++i) {
      ea += 1;
      os << '\n';
      printEA(os, ea);
      os << "  " << syntax[Asm::Directive::NOP];
    }
    return;
  }

  // end special cases
  ////////////////////////////////////////////////////////////////////

  std::string opcode = ascii_str_tolower(inst.mnemonic);
  os << "  " << opcode << ' ';
  printOperandList(os, inst);
}

void PrettyPrinterBase::printEA(std::ostream& os, gtirb::Addr ea) {
  os << syntax[Asm::Style::Tab];
  if (this->debug) {
    os << std::hex << static_cast<uint64_t>(ea) << ": " << std::dec;
  }
}

void PrettyPrinterBase::printOperandList(std::ostream& os,
                                         const cs_insn& inst) {
  cs_x86& detail = inst.detail->x86;
  uint8_t opCount = detail.op_count;

  // Operands are implicit for various MOVS* instructions. But there is also
  // an SSE2 instruction named MOVSD which has explicit operands.
  if ((inst.id == X86_INS_MOVSB || inst.id == X86_INS_MOVSW ||
       inst.id == X86_INS_MOVSD || inst.id == X86_INS_MOVSQ) &&
      inst.detail->groups[0] != X86_GRP_SSE2) {
    opCount = 0;
  }

  for (int i = 0; i < opCount; i++) {
    if (i != 0) {
      os << ',';
    }
    printOperand(os, inst, i);
  }
}

void PrettyPrinterBase::printOperand(std::ostream& os, const cs_insn& inst,
                                     uint64_t index) {
  gtirb::Addr ea(inst.address);
  const gtirb::Module& module = *this->ir.modules().begin();
  const cs_x86_op& op = inst.detail->x86.operands[index];

  const gtirb::SymbolicExpression* symbolic = nullptr;
  uint8_t immOffset = inst.detail->x86.encoding.imm_offset;
  uint8_t dispOffset = inst.detail->x86.encoding.disp_offset;

  switch (op.type) {
  case X86_OP_REG:
    printOpRegdirect(os, inst, op);
    return;
  case X86_OP_IMM: {
    auto found = module.findSymbolicExpression(ea + immOffset);
    if (found != module.symbolic_expr_end())
      symbolic = &*found;
  }
    printOpImmediate(os, symbolic, inst, index);
    return;
  case X86_OP_MEM:
    if (dispOffset > 0) {
      auto found = module.findSymbolicExpression(ea + dispOffset);
      if (found != module.symbolic_expr_end())
        symbolic = &*found;
    }
    printOpIndirect(os, symbolic, inst, index);
    return;
  case X86_OP_INVALID:
    std::cerr << "invalid operand\n";
    exit(1);
  }
}

void PrettyPrinterBase::printDataObject(std::ostream& os,
                                        const gtirb::DataObject& dataObject) {
  gtirb::Addr addr = dataObject.getAddress();
  if (skipEA(addr)) {
    return;
  }
  printComments(os, gtirb::Offset(dataObject.getUUID(), 0),
                dataObject.getSize());
  printSymbolDefinitionsAtAddress(os, addr);
  if (this->debug)
    os << std::hex << static_cast<uint64_t>(addr) << std::dec << ':';
  const auto section = getContainerSection(addr);
  assert(section && "Found a data object outside all sections");
  if (shouldExcludeDataElement(**section, dataObject))
    return;
  gtirb::Module& module = *this->ir.modules().begin();
  auto dataObjectBytes = getBytes(module.getImageByteMap(), dataObject);
  if (dataObjectBytes.empty())
    printZeroDataObject(os, dataObject);
  else
    printNonZeroDataObject(os, dataObject);
}

void PrettyPrinterBase::printNonZeroDataObject(
    std::ostream& os, const gtirb::DataObject& dataObject) {
  gtirb::Module& module = *this->ir.modules().begin();
  const auto& foundSymbolic =
      module.findSymbolicExpression(dataObject.getAddress());
  if (foundSymbolic != module.symbolic_expr_end()) {
    printSymbolicData(os, &*foundSymbolic, dataObject);
    os << '\n';
    return;
  }
  const auto* types =
      module.getAuxData<std::map<gtirb::UUID, std::string>>("encodings");
  if (types) {
    auto foundType = types->find(dataObject.getUUID());
    if (foundType != types->end() && foundType->second == "string") {
      printString(os, dataObject);
      os << '\n';
      return;
    }
  }
  for (std::byte byte : getBytes(module.getImageByteMap(), dataObject)) {
    printByte(os, byte);
  }
}

void PrettyPrinterBase::printZeroDataObject(
    std::ostream& os, const gtirb::DataObject& dataObject) {
  os << " .zero " << dataObject.getSize() << '\n';
}

void PrettyPrinterBase::printComments(std::ostream& os,
                                      const gtirb::Offset& offset,
                                      uint64_t range) {
  if (!this->debug)
    return;

  if (const auto* comments =
          this->ir.modules()
              .begin()
              ->getAuxData<std::map<gtirb::Offset, std::string>>("comments")) {
    gtirb::Offset endOffset(offset.ElementId, offset.Displacement + range);
    for (auto p = comments->lower_bound(offset);
         p != comments->end() && p->first < endOffset; ++p) {
      os << syntax[Asm::Style::Comment];
      if (p->first.Displacement > offset.Displacement)
        os << "+" << p->first.Displacement - offset.Displacement << ":";
      os << " " << p->second << '\n';
    }
  }
}

void PrettyPrinterBase::printCFIDirectives(std::ostream& os,
                                           const gtirb::Offset& offset) {
  const auto* cfiDirectives =
      this->ir.modules()
          .begin()
          ->getAuxData<
              std::map<gtirb::Offset,
                       std::vector<std::tuple<std::string, std::vector<int64_t>,
                                              gtirb::UUID>>>>("cfiDirectives");
  if (!cfiDirectives)
    return;
  const auto entry = cfiDirectives->find(offset);
  if (entry == cfiDirectives->end())
    return;

  for (auto& cfiDirective : entry->second) {
    os << std::get<0>(cfiDirective) << " ";
    const std::vector<int64_t>& operands = std::get<1>(cfiDirective);
    for (auto it = operands.begin(); it != operands.end(); it++) {
      if (it != operands.begin())
        os << ", ";
      os << *it;
    }

    gtirb::Symbol* symbol =
        nodeFromUUID<gtirb::Symbol>(context, std::get<2>(cfiDirective));
    if (symbol) {
      if (operands.size() > 0)
        os << ", ";
      printSymbolReference(os, symbol, true);
    }

    os << std::endl;
  }
}

void PrettyPrinterBase::printSymbolicData(
    std::ostream& os, const gtirb::SymbolicExpression* symbolic,
    const gtirb::DataObject& dataObject) {
  printDataObjectType(os, dataObject);
  os << " ";
  if (const auto* s = std::get_if<gtirb::SymAddrConst>(symbolic)) {
    printSymbolicExpression(os, s, true);
  } else if (const auto* sa = std::get_if<gtirb::SymAddrAddr>(symbolic)) {
    printSymbolicExpression(os, sa, true);
  }
}

void PrettyPrinterBase::printDataObjectType(
    std::ostream& os, const gtirb::DataObject& dataObject) {
  const auto* types =
      this->ir.modules()
          .begin()
          ->getAuxData<std::map<gtirb::UUID, std::string>>("encodings");
  if (types) {
    auto foundType = types->find(dataObject.getUUID());
    if (foundType != types->end()) {
      os << "." << foundType->second;
      return;
    }
  }
  switch (dataObject.getSize()) {
  case 1:
    os << ".byte";
    break;
  case 2:
    os << ".word";
    break;
  case 4:
    os << ".long";
    break;
  case 8:
    os << ".quad";
    break;
  default:
    assert("Data object with unknown type has incompatible size");
    break;
  }
}

void PrettyPrinterBase::printSymbolicExpression(
    std::ostream& os, const gtirb::SymAddrConst* sexpr, bool inData) {
  printSymbolReference(os, sexpr->Sym, inData);
  printAddend(os, sexpr->Offset);
}

void PrettyPrinterBase::printSymbolicExpression(std::ostream& os,
                                                const gtirb::SymAddrAddr* sexpr,
                                                bool inData) {
  printSymbolReference(os, sexpr->Sym1, inData);
  os << '-';
  printSymbolReference(os, sexpr->Sym2, inData);
}

void PrettyPrinterBase::printString(std::ostream& os,
                                    const gtirb::DataObject& x) {
  auto cleanByte = [](uint8_t b) {
    std::string cleaned;
    cleaned += b;
    cleaned = boost::replace_all_copy(cleaned, "\\", "\\\\");
    cleaned = boost::replace_all_copy(cleaned, "\"", "\\\"");
    cleaned = boost::replace_all_copy(cleaned, "\n", "\\n");
    cleaned = boost::replace_all_copy(cleaned, "\t", "\\t");
    cleaned = boost::replace_all_copy(cleaned, "\v", "\\v");
    cleaned = boost::replace_all_copy(cleaned, "\b", "\\b");
    cleaned = boost::replace_all_copy(cleaned, "\r", "\\r");
    cleaned = boost::replace_all_copy(cleaned, "\a", "\\a");
    cleaned = boost::replace_all_copy(cleaned, "\'", "\\'");

    return cleaned;
  };

  os << ".string \"";

  for (const std::byte& b :
       getBytes(this->ir.modules().begin()->getImageByteMap(), x)) {
    if (b != std::byte(0)) {
      os << cleanByte(uint8_t(b));
    }
  }

  os << '"';
}

bool PrettyPrinterBase::shouldExcludeDataElement(
    const gtirb::Section& section, const gtirb::DataObject& dataObject) const {
  if (!skip_data.count(section.getName()))
    return false;
  const gtirb::Module& module = *this->ir.modules().begin();
  auto foundSymbolic = module.findSymbolicExpression(dataObject.getAddress());
  if (foundSymbolic != module.symbolic_expr_end()) {
    if (const auto* s = std::get_if<gtirb::SymAddrConst>(&*foundSymbolic)) {
      return skipEA(*s->Sym->getAddress());
    }
  }
  return false;
}

bool PrettyPrinterBase::skipEA(const gtirb::Addr x) const {
  return !this->debug && (isInSkippedSection(x) || isInSkippedFunction(x));
}

bool PrettyPrinterBase::isInSkippedSection(const gtirb::Addr addr) const {
  if (debug)
    return false;
  const auto section = getContainerSection(addr);
  return section && skip_sects.count((*section)->getName());
}

bool PrettyPrinterBase::isInSkippedFunction(const gtirb::Addr x) const {
  std::optional<std::string> xFunctionName = getContainerFunctionName(x);
  if (!xFunctionName)
    return false;
  return skip_funcs.count(*xFunctionName);
}

std::optional<std::string>
PrettyPrinterBase::getContainerFunctionName(const gtirb::Addr x) const {
  auto it = std::upper_bound(this->functionEntry.begin(),
                             this->functionEntry.end(), x);
  if (it == this->functionEntry.begin())
    return std::nullopt;
  it--;
  return this->getFunctionName(*it);
}

const std::optional<const gtirb::Section*>
PrettyPrinterBase::getContainerSection(const gtirb::Addr addr) const {
  auto found_sections = this->ir.begin()->findSection(addr);
  if (found_sections.begin() == found_sections.end())
    return std::nullopt;
  else
    return &*found_sections.begin();
}

std::string PrettyPrinterBase::getRegisterName(unsigned int reg) const {
  return ascii_str_toupper(
      reg == X86_REG_INVALID ? "" : cs_reg_name(this->csHandle, reg));
}

void PrettyPrinterBase::printAddend(std::ostream& os, int64_t number,
                                    bool first) {
  if (number < 0 || first) {
    os << number;
    return;
  }
  if (number == 0)
    return;
  os << "+" << number;
}

void PrettyPrinterBase::printAlignment(std::ostream& os, gtirb::Addr addr) {
  // Enforce maximum alignment
  uint64_t x{addr};
  if (x % 16 == 0) {
    os << syntax[Asm::Directive::Align] << " 16\n";
    return;
  }
  if (x % 8 == 0) {
    os << syntax[Asm::Directive::Align] << " 8\n";
    return;
  }
  if (x % 4 == 0) {
    os << syntax[Asm::Directive::Align] << " 4\n";
    return;
  }
  if (x % 2 == 0) {
    os << syntax[Asm::Directive::Align] << " 2\n";
    return;
  }
}

std::string PrettyPrinterBase::getFunctionName(gtirb::Addr x) const {
  // Is this address an entry point to a function with a symbol?
  bool entry_point = std::binary_search(this->functionEntry.begin(),
                                        this->functionEntry.end(), x);
  if (entry_point) {
    for (gtirb::Symbol& s : this->ir.modules().begin()->findSymbols(x)) {
      std::stringstream name(s.getName());
      if (isAmbiguousSymbol(s.getName())) {
        name.seekp(0, std::ios_base::end);
        name << '_' << std::hex << static_cast<uint64_t>(x);
      }
      return name.str();
    }
  }

  // Is this a function entry with no associated symbol?
  if (entry_point) {
    std::stringstream name;
    name << "unknown_function_" << std::hex << static_cast<uint64_t>(x);
    return name.str();
  }

  // This doesn't seem to be a function.
  return std::string{};
}

std::string PrettyPrinterBase::GetSymbolToPrint(gtirb::Addr x) {
  std::stringstream ss;
  ss << ".L_" << std::hex << uint64_t(x) << std::dec;
  return ss.str();
}

std::optional<std::string>
PrettyPrinterBase::getForwardedSymbolName(const gtirb::Symbol* symbol,
                                          bool isAbsolute) const {
  const auto* symbolForwarding =
      ir.modules().begin()->getAuxData<std::map<gtirb::UUID, gtirb::UUID>>(
          "symbolForwarding");

  if (symbolForwarding) {
    auto found = symbolForwarding->find(symbol->getUUID());
    if (found != symbolForwarding->end()) {
      gtirb::Node* destSymbol = gtirb::Node::getByUUID(context, found->second);
      return (cast<gtirb::Symbol>(destSymbol))->getName() +
             getForwardedSymbolEnding(symbol, isAbsolute);
    }
  }
  return {};
}

std::string
PrettyPrinterBase::getForwardedSymbolEnding(const gtirb::Symbol* symbol,
                                            bool isAbsolute) const {
  if (symbol->getAddress()) {
    gtirb::Addr addr = *symbol->getAddress();
    const auto container_sections =
        this->ir.modules().begin()->findSection(addr);
    if (container_sections.begin() == container_sections.end())
      return std::string{};
    std::string section_name = container_sections.begin()->getName();
    if (!isAbsolute && (section_name == ".plt" || section_name == ".plt.got"))
      return std::string{"@PLT"};
    if (section_name == ".got" || section_name == ".got.plt")
      return std::string{"@GOTPCREL"};
  }
  return std::string{};
}

bool PrettyPrinterBase::isAmbiguousSymbol(const std::string& name) const {
  // Are there multiple symbols with this name?
  auto found = this->ir.modules().begin()->findSymbols(name);
  return distance(begin(found), end(found)) > 1;
}

std::string PrettyPrinterBase::GetSizeName(uint64_t x) {
  return GetSizeName(std::to_string(x));
}

std::string PrettyPrinterBase::GetSizeName(const std::string& x) {
  static const std::map<std::string, std::string> adapt{
      {"128", ""},         {"0", ""},           {"80", "TBYTE PTR"},
      {"64", "QWORD PTR"}, {"32", "DWORD PTR"}, {"16", "WORD PTR"},
      {"8", "BYTE PTR"}};

  if (const auto found = adapt.find(x); found != std::end(adapt)) {
    return found->second;
  }

  assert("Unknown Size");

  return x;
}

std::string PrettyPrinterBase::GetSizeSuffix(uint64_t x) {
  return GetSizeSuffix(std::to_string(x));
}

std::string PrettyPrinterBase::GetSizeSuffix(const std::string& x) {
  static const std::map<std::string, std::string> adapt{
      {"128", ""}, {"0", ""},   {"80", "t"}, {"64", "q"},
      {"32", "d"}, {"16", "w"}, {"8", "b"}};

  if (const auto found = adapt.find(x); found != std::end(adapt)) {
    return found->second;
  }

  assert("Unknown Size");

  return x;
}

std::string PrettyPrinterBase::AvoidRegNameConflicts(const std::string& x) {
  const std::vector<std::string> adapt{"FS",  "MOD", "DIV", "NOT", "mod", "div",
                                       "not", "and", "or",  "shr", "Si"};

  if (const auto found = std::find(std::begin(adapt), std::end(adapt), x);
      found != std::end(adapt)) {
    return x + "_renamed";
  }

  return x;
}

} // namespace gtirb_pprint
