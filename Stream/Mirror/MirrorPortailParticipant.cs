using System;
using Mirror;
using Portail.Core;
using UnityEngine;

namespace Portail.Stream.Mirror
{
	[DisallowMultipleComponent]
	[RequireComponent(typeof(NetworkIdentity))]
	[RequireComponent(typeof(PortailFeed))]
	public sealed class MirrorPortailParticipant : NetworkBehaviour
	{
		[SyncVar(hook = nameof(OnOwnerStreamIdChanged))]
		ulong _ownerStreamId;

		[SyncVar(hook = nameof(OnBroadcastingChanged))]
		bool _isBroadcasting;

		PortailFeed _feed;

		public ulong OwnerStreamId => _ownerStreamId;
		public bool IsBroadcasting => _isBroadcasting;
		public bool IsLocalParticipant => isLocalPlayer;
		public PortailFeed Feed => EnsureFeed();

		public event Action<MirrorPortailParticipant, ulong> OwnerStreamIdUpdated;
		public event Action<MirrorPortailParticipant, bool> BroadcastingUpdated;

		void Awake()
		{
			EnsureFeed();
		}

		public override void OnStartClient()
		{
			base.OnStartClient();
			EnsureFeed();
		}

		public void TryPublishOwnerStreamId(ulong streamId)
		{
			if (!isLocalPlayer || streamId == 0 || _ownerStreamId == streamId)
				return;

			CmdSetOwnerStreamId(streamId);
		}

		public void TryPublishBroadcasting(bool isBroadcasting)
		{
			if (!isLocalPlayer || _isBroadcasting == isBroadcasting)
				return;

			CmdSetBroadcasting(isBroadcasting);
		}

		PortailFeed EnsureFeed()
		{
			if (_feed != null)
				return _feed;

			_feed = GetComponent<PortailFeed>();
			if (_feed == null)
				_feed = gameObject.AddComponent<PortailFeed>();

			return _feed;
		}

		[Command]
		void CmdSetOwnerStreamId(ulong streamId)
		{
			if (streamId == 0)
				return;

			_ownerStreamId = streamId;
		}

		[Command]
		void CmdSetBroadcasting(bool isBroadcasting)
		{
			_isBroadcasting = isBroadcasting;
		}

		void OnOwnerStreamIdChanged(ulong _, ulong ownerStreamId)
		{
			OwnerStreamIdUpdated?.Invoke(this, ownerStreamId);
		}

		void OnBroadcastingChanged(bool _, bool isBroadcasting)
		{
			BroadcastingUpdated?.Invoke(this, isBroadcasting);
		}
	}
}
