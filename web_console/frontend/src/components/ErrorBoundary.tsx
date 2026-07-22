import { Component, ReactNode } from 'react'

interface Props { children: ReactNode }
interface State { error: Error | null }

export default class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null }

  static getDerivedStateFromError(error: Error): State {
    return { error }
  }

  render() {
    if (this.state.error) {
      return (
        <div style={{
          padding: 40, color: '#fca5a5', fontFamily: 'monospace',
          background: '#0f1117', minHeight: '100vh'
        }}>
          <h2 style={{ color: '#ef4444', marginBottom: 12 }}>页面渲染出错</h2>
          <pre style={{ fontSize: 12, whiteSpace: 'pre-wrap', color: '#94a3b8' }}>
            {this.state.error.message}
            {'\n\n'}
            {this.state.error.stack}
          </pre>
          <button
            style={{ marginTop: 20, padding: '8px 20px', background: '#1e3a6b', border: '1px solid #4f8ef7', color: '#93c5fd', borderRadius: 6, cursor: 'pointer' }}
            onClick={() => { this.setState({ error: null }); window.history.back() }}
          >← 返回上一页</button>
        </div>
      )
    }
    return this.props.children
  }
}
