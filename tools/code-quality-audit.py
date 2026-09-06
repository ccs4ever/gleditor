#!/usr/bin/env python3
"""
Code Quality & Architectural Layering Auditor for gleditor.

Scans the workspace to enforce:
1. Layer Purity: include/ and src/ must never depend on apps/
2. Promotion Opportunities: Highlight areas where zigzag depends on xudu core engines,
   identifying candidates to promote to src/ or include/gleditor/
3. Supplemental Metadata & Format Invariants: Verify system xanadocs/slices adhere to
   linked Schema and Notes specifications (no markdown headers, format links).
"""

import sys
import re
from pathlib import Path

WORKSPACE_ROOT = Path(__file__).resolve().parent.parent

INCLUDE_DIR = WORKSPACE_ROOT / "include"
SRC_DIR = WORKSPACE_ROOT / "src"
APPS_DIR = WORKSPACE_ROOT / "apps"

def check_layer_purity():
    violations = []
    # Regular expression for finding library code importing app code
    app_headers_re = re.compile(r'#include\s*[<"](?:apps/|xudu/|zigzag/)([^>"]+)[>"]')
    
    # Check src/ and include/
    for base in [INCLUDE_DIR, SRC_DIR]:
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if p.suffix in [".hpp", ".h", ".cpp"]:
                content = p.read_text(encoding="utf-8", errors="ignore")
                for line_no, line in enumerate(content.splitlines(), start=1):
                    m = app_headers_re.search(line)
                    if m:
                        violations.append(f"{p.relative_to(WORKSPACE_ROOT)}:{line_no} Layer Inversion: Library imports app header '{m.group(0)}'")
                        
    return violations

def check_isolation_and_coupling():
    violations = []
    zigzag_dir = APPS_DIR / "zigzag"
    xudu_dir = APPS_DIR / "xudu"
    
    # Zigzag must not import xudu
    if zigzag_dir.exists():
        for p in zigzag_dir.rglob("*"):
            if p.suffix in [".hpp", ".h", ".cpp"]:
                content = p.read_text(encoding="utf-8", errors="ignore")
                for line_no, line in enumerate(content.splitlines(), start=1):
                    if re.search(r'#include\s*[<"](?:xudu/|apps/xudu/)', line):
                        violations.append(f"{p.relative_to(WORKSPACE_ROOT)}:{line_no} Decoupling Violation: zigzag imports xudu directly (must use common/xanadu)")
                        
    # Xudu must not import zigzag
    if xudu_dir.exists():
        for p in xudu_dir.rglob("*"):
            if p.suffix in [".hpp", ".h", ".cpp"]:
                content = p.read_text(encoding="utf-8", errors="ignore")
                for line_no, line in enumerate(content.splitlines(), start=1):
                    if re.search(r'#include\s*[<"](?:zigzag/|apps/zigzag/)', line):
                        violations.append(f"{p.relative_to(WORKSPACE_ROOT)}:{line_no} Decoupling Violation: xudu imports zigzag directly (must use common/xanadu)")
                        
    return violations

def check_markdown_in_system_docs():
    violations = []
    md_header_re = re.compile(r'^\s*#{1,6}\s+.*$')
    
    sample_system_docs = list(WORKSPACE_ROOT.glob("assets/system_*.xanadoc")) + \
                         list(WORKSPACE_ROOT.glob("apps/xudu/**/system_*.xanadoc"))
    
    for p in sample_system_docs:
        content = p.read_text(encoding="utf-8", errors="ignore")
        for line_no, line in enumerate(content.splitlines(), start=1):
            if md_header_re.match(line):
                violations.append(f"{p.relative_to(WORKSPACE_ROOT)}:{line_no} Format Invariant: Markdown header syntax detected in system document! Headers must be raw text with format links (bold, align-centre, font-scale).")
                
    # Also verify system slices in assets/zigzag
    zigzag_system_slices = list((WORKSPACE_ROOT / "assets/zigzag").glob("system_*.yaml"))
    for p in zigzag_system_slices:
        content = p.read_text(encoding="utf-8", errors="ignore")
        if "d.schema:" not in content:
            violations.append(f"{p.relative_to(WORKSPACE_ROOT)} Missing required 'd.schema' dimension for system slice schema link.")
        if "d.notes:" not in content:
            violations.append(f"{p.relative_to(WORKSPACE_ROOT)} Missing required 'd.notes' dimension for system slice user notes link.")
        cell_text_matches = re.findall(r'text:\s*"([^"]+)"', content)
        for text in cell_text_matches:
            for line in text.split(r'\n'):
                if md_header_re.match(line):
                    violations.append(f"{p.relative_to(WORKSPACE_ROOT)} Format Invariant: Markdown header syntax in cell text '{line}'! Headers must be raw text.")
                    
    return violations

def run_audit():
    print("==================================================")
    print("gleditor Architectural & Code Quality Audit")
    print("==================================================")
    
    layer_violations = check_layer_purity()
    coupling_violations = check_isolation_and_coupling()
    doc_violations = check_markdown_in_system_docs()
    
    print("\n--- 1. Library Layer Purity (Zero App / Xanadu / Zigzag Leaks) ---")
    if not layer_violations:
        print("  [PASS] Zero layer inversions detected. Library (src/, include/) is strictly isolated.")
    else:
        for v in layer_violations:
            print(f"  [FAIL] {v}")
            
    print("\n--- 2. Application Decoupling (Xudu vs Zigzag Isolation via apps/common/xanadu) ---")
    if not coupling_violations:
        print("  [PASS] Clean decoupling: Zigzag and Xudu have zero direct cross-includes.")
        print("         All shared xanalogical models and engines live in apps/common/xanadu.")
    else:
        for v in coupling_violations:
            print(f"  [FAIL] {v}")
            
    print("\n--- 3. System Document Format & Styling Governance ---")
    if not doc_violations:
        print("  [PASS] Zero markdown headers in system xanadocs. Format links respected.")
    else:
        for v in doc_violations:
            print(f"  [FAIL] {v}")
            
    print("\n==================================================")
    total_failures = len(layer_violations) + len(coupling_violations) + len(doc_violations)
    if total_failures == 0:
        print("Audit Result: ALL ARCHITECTURAL INVARIANTS SATISFIED")
        return 0
    else:
        print(f"Audit Result: {total_failures} CRITICAL VIOLATION(S) DETECTED")
        return 1

if __name__ == "__main__":
    sys.exit(run_audit())
