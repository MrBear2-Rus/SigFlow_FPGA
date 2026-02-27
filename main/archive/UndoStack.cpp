#include "UndoStack.h"
#include "CanvasPanel.h"
#include "UndoNotifier.h"

class CanvasElement;
class Wire;
class CanvasTextElement;

/* ---------------- CmdAddElement ---------------- */
void CmdAddElement::undo(CanvasPanel* canvas)
{
    if (m_index < canvas->GetElements().size())
    {
        canvas->DeleteElement(m_index);
    }
}

wxString CmdAddElement::GetName() const
{
    return wxString("Add ") + m_name;
}

/* ---------------- UndoStack ---------------- */
void UndoStack::Push(std::unique_ptr<Command> cmd)
{
    m_stack.emplace_back(std::move(cmd));
    if (m_stack.size() > m_limit)
        m_stack.erase(m_stack.begin());
    // ����֪ͨ
    UndoNotifier::Notify(GetUndoName(), CanUndo());
}

void UndoStack::Undo(CanvasPanel* canvas)
{
    if (!CanUndo()) return;
    auto& top = m_stack.back();
    top->undo(canvas);
    m_stack.pop_back();
}

wxString UndoStack::GetUndoName() const
{
    return CanUndo() ? m_stack.back()->GetName() : wxString("Can't Undo");
}

void CmdAddWire::undo(CanvasPanel* canvas)
{
    if (m_wireIdx < canvas->GetWires().size())
    {
        canvas->DeleteWire(m_wireIdx);
    }
}

void CmdAddText::undo(CanvasPanel* canvas)
{
    if (m_textIdx < canvas->GetTextElements().size())
    {
        canvas->DeleteTextElement(m_textIdx);
    }
}

void CmdMoveSelected::undo(CanvasPanel* canvas) {
    for (int i = 0; i < m_textElemIdx.size(); i++) {
        canvas->TextSetPos(m_textElemIdx[i], m_textElemPos[i]);
    }
    for (int i = 0; i < m_compntIdx.size(); i++) {
        canvas->ElementSetPos(m_compntIdx[i], m_compntPos[i]);

        for (const auto& aw : m_movingWires[i]) {
            if (aw.wireIdx >= canvas->GetWires().size()) continue;
            auto it = std::find(m_wireIdx.begin(), m_wireIdx.end(), aw.wireIdx);
            const Wire& wire = canvas->GetWires()[aw.wireIdx];

            // 计算新引脚世界坐标
            const auto& elem = canvas->GetElements()[m_compntIdx[i]];
            const auto& pins = aw.isInput ? elem.GetInputPins() : elem.GetOutputPins();
            if (aw.pinIdx >= pins.size()) continue;
            wxPoint pinOffset = wxPoint(pins[aw.pinIdx].pos.x, pins[aw.pinIdx].pos.y);
            wxPoint newPinPos = elem.GetPos() + pinOffset;

            Wire tmp_wire = wire;
            // 更新导线端点
            if (aw.ptIdx == 0) {
                tmp_wire.pts.front().pos = newPinPos;
                tmp_wire.pts[1].pos.y = newPinPos.y;
            }
            else {
                tmp_wire.pts.back().pos = newPinPos;
                tmp_wire.pts[tmp_wire.pts.size() - 2].pos.y = newPinPos.y;
            }
            canvas->UpdateWire(tmp_wire, aw.wireIdx);
        }

    }
    for (int i = 0; i < m_wireIdx.size(); i++) {
        for (int j = 0; j < canvas->GetWires()[m_wireIdx[i]].Size(); j++) {
            canvas->WirePtsSetPos(m_wireIdx[i], j, m_wirePos[i][j]);
        }
    }


}

void CmdDeleteSelected::undo(CanvasPanel* canvas) {
    for (int i = 0; i < Elements.size(); i++) {
        canvas->ReclaimElement(*Elements[i].elem, Elements[i].idx);
    }
    for (int i = 0; i < Wires.size(); i++) {
        canvas->ReclaimWire(*Wires[i].wire, Wires[i].idx);
    }
    /*for (int i = 0; i < Texts.size(); i++) {
        canvas->ReclaimText(*Texts[i].txt, Texts[i].idx);
    }*/
}

CmdDeleteSelected::CmdDeleteSelected(
    std::vector<CanvasElement> elements,
    std::vector<int> elementIdx,
    std::vector<Wire> wires,
    std::vector<int> wireIdx,
    std::vector<CanvasTextElement> txtBoxes,
    std::vector<int> txtBoxIdx)
{
    // 处理元件
    for (size_t i = 0; i < elements.size(); ++i) {
        Element_Re e;
        e.elem = std::make_unique<CanvasElement>(std::move(elements[i]));
        e.idx = elementIdx[i];
        Elements.push_back(std::move(e));
    }
    std::sort(Elements.begin(), Elements.end(),
        [](const auto& a, const auto& b) { return a.idx < b.idx; });

    // 处理导线 —— 最关键！这里 std::move(wires[i]) 需要 Wire 有移动构造函数
    for (size_t i = 0; i < wires.size(); ++i) {
        Wire_Re w;
        w.wire = std::make_unique<Wire>(std::move(wires[i]));
        w.idx = wireIdx[i];
        Wires.push_back(std::move(w));
    }
    std::sort(Wires.begin(), Wires.end(),
        [](const auto& a, const auto& b) { return a.idx < b.idx; });

    // 处理文本框
    for (size_t i = 0; i < txtBoxes.size(); ++i) {
        Text_Re t;
        t.txt = std::make_unique<CanvasTextElement>(std::move(txtBoxes[i]));
        t.idx = txtBoxIdx[i];
        Texts.push_back(std::move(t));
    }
    std::sort(Texts.begin(), Texts.end(),
        [](const auto& a, const auto& b) { return a.idx < b.idx; });
}
