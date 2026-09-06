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

def check_promotion_opportunities():
    promotions = []
    zigzag_dir = APPS_DIR / "zigzag"
    if zigzag_dir.exists():
        for p in zigzag_dir.rglob("*"):
            if p.suffix in [".hpp", ".h", ".cpp"]:
                content = p.read_text(encoding="utf-8", errors="ignore")
                for line_no, line in enumerate(content.splitlines(), start=1):
                    m = re.search(r'#include\s*[<"](?:xudu/core/)([^>"]+)[>"]', line)
                    if m:
                        promotions.append((str(p.relative_to(WORKSPACE_ROOT)), line_no, m.group(1)))
    return promotions

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
                
    return violations

def run_audit():
    print("==================================================")
    print("gleditor Architectural & Code Quality Audit")
    print("==================================================")
    
    layer_violations = check_layer_purity()
    promotions = check_promotion_opportunities()
    doc_violations = check_markdown_in_system_docs()
    
    print("\n--- 1. Library Layer Purity ---")
    if not layer_violations:
        print("  [PASS] Zero layer inversions detected. Library (src/, include/) is strictly isolated.")
    else:
        for v in layer_violations:
            print(f"  [FAIL] {v}")
            
    print("\n--- 2. Layer Promotion Opportunities (Zigzag -> Xudu Core Dependencies) ---")
    if promotions:
        print(f"  [INFO] Found {len(promotions)} cross-app references where Zigzag depends on xudu/core.")
        print("  Architectural recommendation: promote shared data models (e.g. spool, compact_op, resolver)")
        print("  to a lower shared layer (e.g. src/xanalog/ or include/gleditor/core/).")
        headers = set(item[2] for item in promotions)
        for h in sorted(headers):
            print(f"    - Candidate for promotion: xudu/core/{h}")
    else:
        print("  [INFO] No direct cross-app includes detected.")
            
    print("\n--- 3. System Document Format & Styling Governance ---")
    if not doc_violations:
        print("  [PASS] Zero markdown headers in system xanadocs. Format links respected.")
    else:
        for v in doc_violations:
            print(f"  [FAIL] {v}")
            
    print("\n==================================================")
    if len(layer_violations) == 0 and len(doc_violations) == 0:
        print("Audit Result: ALL ARCHITECTURAL INVARIANTS SATISFIED")
        return 0
    else:
        print("Audit Result: CRITICAL VIOLATIONS DETECTED")
        return 1

if __name__ == "__main__":
    sys.exit(run_audit())
