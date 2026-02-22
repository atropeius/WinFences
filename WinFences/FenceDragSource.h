#pragma once
// ============================================================================
// FenceDragSource.h — OLE IDropSource for dragging icons OUT of a fence.
//
// Used when the user drags an icon from a fence to the desktop or another app.
// Implements IDropSource::QueryContinueDrag and GiveFeedback.
// The actual data object (IDataObject) is provided by the shell via
// IShellFolder::GetUIObjectOf.
// ============================================================================
#include "pch.h"

// ---------------------------------------------------------------------------
// FenceDragSource  –  IDropSource implementation
// ---------------------------------------------------------------------------
// Used when dragging an icon OUT of a fence.
// Provides drag feedback and reports when the drag is cancelled or completed.
// ---------------------------------------------------------------------------

class FenceDragSource : public IDropSource
{
public:
    FenceDragSource() : m_refCount(1) {}

    // ---- IUnknown ----
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = InterlockedDecrement(&m_refCount);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropSource)
        {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // ---- IDropSource ----

    // Called continuously during drag - return S_OK to continue, DRAGDROP_S_CANCEL to abort
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(
        BOOL fEscapePressed, DWORD grfKeyState) override
    {
        if (fEscapePressed)
            return DRAGDROP_S_CANCEL;

        // Drop when left mouse button released
        if (!(grfKeyState & MK_LBUTTON))
            return DRAGDROP_S_DROP;

        return S_OK;
    }

    // Called to give visual feedback during drag
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD /*dwEffect*/) override
    {
        // Use default system cursor feedback
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    LONG m_refCount;
};

// ---------------------------------------------------------------------------
// Helper: build an IDataObject for a single file path (CF_HDROP)
// ---------------------------------------------------------------------------
// Used to initiate DoDragDrop with the icon's file path.

inline HRESULT CreateFileDataObject(const std::wstring& path, IDataObject** ppDataObj)
{
    // Use shell's SHCreateDataObject via IShellItem for proper shell integration
    ComPtr<IShellItem> pItem;
    HRESULT hr = SHCreateItemFromParsingName(
        path.c_str(), nullptr, IID_PPV_ARGS(&pItem));
    if (FAILED(hr)) return hr;

    // Get IDataObject from the shell item
    hr = pItem->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(ppDataObj));
    return hr;
}
